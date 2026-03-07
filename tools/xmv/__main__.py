"""
Xbox XMV Video Demuxer & Converter

Parses Microsoft Xbox XMV container format used by original Xbox games.
XMV contains WMV2 video (little-endian bitstream) + Xbox ADPCM audio.

Usage:
    py -3 -m tools.xmv info <file.xmv>           Show file details
    py -3 -m tools.xmv extract <file.xmv> [-o d]  Extract raw streams
    py -3 -m tools.xmv convert <file.xmv> [-o f]  Convert to MP4 via FFmpeg
    py -3 -m tools.xmv batch <dir> [-o dir]        Convert all XMV in directory
"""

import sys
import argparse
from pathlib import Path
from .xmv_demux import XMVFile, xmv_info, xmv_extract, xmv_convert, xmv_batch


def main():
    parser = argparse.ArgumentParser(
        prog='tools.xmv',
        description='Xbox XMV video demuxer and converter')
    sub = parser.add_subparsers(dest='command')

    # info
    p_info = sub.add_parser('info', help='Show XMV file details')
    p_info.add_argument('file', help='XMV file to inspect')

    # extract
    p_ext = sub.add_parser('extract', help='Extract raw video/audio streams')
    p_ext.add_argument('file', help='XMV file to extract')
    p_ext.add_argument('-o', '--output-dir', default='.', help='Output directory')

    # convert
    p_conv = sub.add_parser('convert', help='Convert XMV to MP4 using FFmpeg')
    p_conv.add_argument('file', help='XMV file to convert')
    p_conv.add_argument('-o', '--output', help='Output file (default: <name>.mp4)')
    p_conv.add_argument('--ffmpeg', default='ffmpeg', help='Path to ffmpeg binary')

    # batch
    p_batch = sub.add_parser('batch', help='Convert all XMV files in a directory')
    p_batch.add_argument('dir', help='Directory containing XMV files')
    p_batch.add_argument('-o', '--output-dir', help='Output directory (default: same)')
    p_batch.add_argument('--ffmpeg', default='ffmpeg', help='Path to ffmpeg binary')

    args = parser.parse_args()

    if args.command is None:
        parser.print_help()
        return

    if args.command == 'info':
        xmv_info(args.file)
    elif args.command == 'extract':
        xmv_extract(args.file, args.output_dir)
    elif args.command == 'convert':
        xmv_convert(args.file, args.output, getattr(args, 'ffmpeg', 'ffmpeg'))
    elif args.command == 'batch':
        xmv_batch(args.dir, args.output_dir, getattr(args, 'ffmpeg', 'ffmpeg'))


if __name__ == '__main__':
    main()
