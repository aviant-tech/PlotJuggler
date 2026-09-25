"""Compare CSVs written by bin_log_dump_csv with the vendor parser's CSVs.

    python3 compare_with_vendor.py VENDOR_DIR MINE_DIR

Both directories hold <base>_<topic>.csv files. Timestamps are compared after applying the vendor's
float32 rounding, numbers with a 1e-6 relative tolerance (the vendor prints 6 decimals), and the
mavlink "info" column is skipped (the decoder emits the event fields as separate columns instead).
"""
import csv
import glob
import os
import struct
import sys


def vendor_timestamp(seconds):
    """The vendor converts the microsecond count to float32 before printing."""
    micros = struct.unpack('f', struct.pack('f', round(seconds * 1e6)))[0]
    return struct.unpack('f', struct.pack('f', micros / 1e6))[0]


def number(cell):
    try:
        return float(cell.split(' ')[0])  # labels like "1 (power on)"
    except ValueError:
        return None


def cells_match(name, vendor, mine):
    if name == 'timestamp':
        return '%.5f' % vendor_timestamp(float(mine)) == vendor
    if 'hex' in name:
        return int(vendor, 16) == int(mine)
    if vendor == '' or mine == '':
        return vendor == mine
    x, y = number(vendor), number(mine)
    if x is None or y is None:
        return vendor == mine
    return abs(x - y) <= max(abs(x) * 1e-6, 5.01e-7)


def main():
    vendor_dir, mine_dir = sys.argv[1:3]
    all_ok = True
    for vendor_file in sorted(glob.glob(os.path.join(vendor_dir, '*.csv'))):
        name = os.path.basename(vendor_file)
        with open(vendor_file) as f:
            vendor_rows = list(csv.reader(f))
        with open(os.path.join(mine_dir, name)) as f:
            mine_rows = list(csv.reader(f))
        vendor_header, mine_header = vendor_rows[0], mine_rows[0]
        vendor_rows, mine_rows = vendor_rows[1:], mine_rows[1:]
        issues = []
        if len(vendor_rows) != len(mine_rows):
            issues.append('rows %d vs %d' % (len(vendor_rows), len(mine_rows)))
        for column, field in enumerate(vendor_header):
            if field == 'info':
                continue
            if field not in mine_header:
                issues.append('missing column %r' % field)
                continue
            mine_column = mine_header.index(field)
            bad = [(i, v[column], m[mine_column])
                   for i, (v, m) in enumerate(zip(vendor_rows, mine_rows))
                   if not cells_match(field, v[column], m[mine_column])]
            if bad:
                issues.append('%r: %d mismatches e.g. %s' % (field, len(bad), bad[0]))
        all_ok = all_ok and not issues
        print('%-45s %s' % (name, 'OK' if not issues else '; '.join(issues)))
    print('ALL OK' if all_ok else 'DIFFERENCES')
    sys.exit(0 if all_ok else 1)


if __name__ == '__main__':
    main()
