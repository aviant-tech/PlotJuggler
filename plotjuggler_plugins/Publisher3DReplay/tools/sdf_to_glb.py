#!/usr/bin/env python3
"""
Flatten the visuals of a Gazebo SDF model into a single glTF binary (.glb)
for the 3D replay page.

The model built into the plugin is Notus 4, from aviant-tech/notus-simulator
branch feat/gz-harmonic-notus4 (dockerfiles/px4/gz/models/notus4), without
its payload:

  git clone -b feat/gz-harmonic-notus4 https://github.com/aviant-tech/notus-simulator.git
  pip install trimesh pycollada scipy
  ./sdf_to_glb.py --exclude-link package \
      notus-simulator/dockerfiles/px4/gz/models/notus4/model.sdf ../web/notus4.glb

The payload is a model of its own, so the page can hang it from the winch line:

  ./sdf_to_glb.py --only-link package \
      notus-simulator/dockerfiles/px4/gz/models/notus4/model.sdf ../web/package.glb

SDF links and visuals are in a forward-left-up frame. Cesium expects glTF's
convention (+Z forward, +Y up, +X left), so vertices are written as
(y, z, x). Links are placed at their model-frame pose. Rotors (the links of
the motor model plugins) are nodes of their own, named after the link, with
glTF extras {"motor": n, "direction": 1 for ccw, -1 for cw}, for the page to
spin them; control surfaces are shown in their rest position. Visuals a plugin
parks far away until it needs them (Notus' winch line) are left out.
"""
import argparse
import json
import os
import struct
import xml.etree.ElementTree as ET

import numpy as np
import trimesh
from scipy.spatial.transform import Rotation

# visuals posed further away than this are hidden, not part of the airframe
PARKED_DISTANCE = 1000.0

FLU_TO_GLTF = np.array([[0, 1, 0, 0],
                        [0, 0, 1, 0],
                        [1, 0, 0, 0],
                        [0, 0, 0, 1]], dtype=float)


def pose_matrix(text):
    """ SDF <pose>x y z roll pitch yaw</pose> as a 4x4 transform """
    values = [float(v) for v in (text or '0 0 0 0 0 0').split()]
    matrix = np.eye(4)
    matrix[:3, :3] = Rotation.from_euler('xyz', values[3:]).as_matrix()
    matrix[:3, 3] = values[:3]
    return matrix


def load_geometry(geometry, model_dir):
    """ trimesh for an SDF <geometry> element, in the visual's frame """
    mesh = geometry.find('mesh')
    if mesh is not None:
        uri = mesh.findtext('uri')
        path = os.path.join(model_dir, uri.split('/', 3)[3])
        loaded = trimesh.load(path, force='scene').to_geometry()
        scale = [float(v) for v in (mesh.findtext('scale') or '1 1 1').split()]
        loaded.apply_scale(scale)
        return loaded
    box = geometry.find('box')
    if box is not None:
        return trimesh.creation.box(extents=[float(v) for v in box.findtext('size').split()])
    cylinder = geometry.find('cylinder')
    if cylinder is not None:
        return trimesh.creation.cylinder(radius=float(cylinder.findtext('radius')),
                                         height=float(cylinder.findtext('length')),
                                         sections=24)
    raise ValueError('unsupported geometry: ' + ', '.join(child.tag for child in geometry))


def pbr_material(visual):
    """ glTF material from an SDF <material>: diffuse colour, <pbr><metal> and emissive """
    def values(path, default):
        return [float(v) for v in (visual.findtext(path) or default).split()]
    emissive = values('material/emissive', '0 0 0 1')[:3]
    return trimesh.visual.material.PBRMaterial(
        baseColorFactor=values('material/diffuse', '0.7 0.7 0.7 1'),
        metallicFactor=values('material/pbr/metal/metalness', '0.1')[0],
        roughnessFactor=values('material/pbr/metal/roughness', '0.7')[0],
        emissiveFactor=emissive if any(emissive) else None,
        doubleSided=True)


def rotor_links(model):
    """ link name -> {"motor": n, "direction": 1 (ccw) or -1 (cw)} from the motor model plugins """
    rotors = {}
    for plugin in model.findall('plugin'):
        if plugin.findtext('linkName') and plugin.findtext('motorNumber'):
            rotors[plugin.findtext('linkName')] = {
                'motor': int(plugin.findtext('motorNumber')),
                'direction': -1 if plugin.findtext('turningDirection') == 'cw' else 1,
            }
    return rotors


def set_node_extras(glb_path, extras):
    """ Add glTF extras to the nodes named in extras (trimesh cannot) """
    with open(glb_path, 'rb') as f:
        data = f.read()
    json_length = struct.unpack_from('<I', data, 12)[0]
    gltf = json.loads(data[20:20 + json_length])
    for node in gltf['nodes']:
        if node.get('name') in extras:
            node['extras'] = extras[node['name']]
    chunk = json.dumps(gltf, separators=(',', ':')).encode()
    chunk += b' ' * (-len(chunk) % 4)
    rest = data[20 + json_length:]
    with open(glb_path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(chunk) + len(rest)))
        f.write(struct.pack('<II', len(chunk), 0x4E4F534A))
        f.write(chunk + rest)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sdf')
    parser.add_argument('glb')
    parser.add_argument('--exclude-link', action='append', default=[], metavar='NAME',
                        help='leave out a link, e.g. a payload (repeatable)')
    parser.add_argument('--only-link', metavar='NAME',
                        help='export just this link, in its own frame (e.g. the payload)')
    args = parser.parse_args()

    model_dir = os.path.dirname(os.path.abspath(args.sdf))
    model = ET.parse(args.sdf).getroot().find('model')
    rotors = rotor_links(model)

    scene = trimesh.Scene()
    for link in model.findall('link'):
        name = link.get('name')
        if name in args.exclude_link or (args.only_link and name != args.only_link):
            continue
        link_pose = np.eye(4) if args.only_link else pose_matrix(link.findtext('pose'))
        for visual in link.findall('visual'):
            visual_pose = pose_matrix(visual.findtext('pose'))
            if np.linalg.norm((link_pose @ visual_pose)[:3, 3]) > PARKED_DISTANCE:
                continue
            mesh = load_geometry(visual.find('geometry'), model_dir)
            mesh.visual = trimesh.visual.TextureVisuals(material=pbr_material(visual))
            if name in rotors:
                # a node of its own, its local Z the shaft (the joint axis), for the page to spin
                mesh.apply_transform(visual_pose)
                scene.add_geometry(mesh, geom_name=visual.get('name'), node_name=name,
                                   transform=FLU_TO_GLTF @ link_pose)
            else:
                mesh.apply_transform(FLU_TO_GLTF @ link_pose @ visual_pose)
                scene.add_geometry(mesh, geom_name=visual.get('name'))

    scene.export(args.glb)
    set_node_extras(args.glb, rotors)
    print('wrote {} ({} visuals, {} rotors, {} bytes)'.format(
        args.glb, len(scene.geometry), len(set(rotors) & set(scene.graph.nodes)),
        os.path.getsize(args.glb)))


if __name__ == '__main__':
    main()
