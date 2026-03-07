"""
Xbox XMV container format parser.

XMV (Xbox Media Video) is the FMV container used on the original Xbox.
It wraps WMV2 video frames (with a little-endian bitstream, unlike standard
WMV2 which is big-endian) and Xbox ADPCM or WMA audio streams.

Format documented from FFmpeg's libavformat/xmv.c by Sven Hesse and
Matthew Hoops, plus independent analysis of retail Xbox game files.

File layout:
    [File Header]
        +0x00  uint32  next_packet_size
        +0x04  uint32  this_packet_size (offset to first packet data)
        +0x08  uint32  max_packet_size
        +0x0C  char[4] "xobX" magic (Xbox backwards)
        +0x10  uint32  version (2 or 4)
        +0x14  uint32  video_width
        +0x18  uint32  video_height
        +0x1C  uint32  video_duration_ms
        +0x20  uint16  audio_track_count
        +0x22  uint16  (padding)
        For each audio track (12 bytes):
            +0x00  uint16  compression (codec tag, 0x0069=Xbox ADPCM, 0x0161=WMA)
            +0x02  uint16  channels
            +0x04  uint32  sample_rate
            +0x08  uint16  bits_per_sample
            +0x0A  uint16  flags

    [Packet] (repeated until EOF)
        +0x00  uint32  next_packet_size
        Video header (8 bytes):
            bits[0:22]   video_data_size
            bits[23:30]  frame_count
            bit[31]      has_extradata
            bytes[4:7]   (reserved)
        For each audio track (4 bytes):
            bits[0:22]   audio_data_size
        [video extradata, 4 bytes, if has_extradata]
        [video frame data]
        [audio data for each track]

    Video frames within packet data:
        +0x00  uint32  frame_header
            bits[0:16]   frame_size_words (actual size = val*4 + 4)
            bits[17:31]  frame_timestamp_delta
        [frame_size bytes of WMV2 bitstream, little-endian]
"""

import struct
import subprocess
import sys
from pathlib import Path


COMPRESSION_NAMES = {
    0x0001: 'PCM',
    0x0002: 'MS ADPCM',
    0x0011: 'IMA ADPCM',
    0x0069: 'Xbox ADPCM',
    0x0161: 'WMAv2',
    0x0162: 'WMA Pro',
}


class XMVAudioTrack:
    __slots__ = ('compression', 'channels', 'sample_rate',
                 'bits_per_sample', 'flags')

    def __init__(self, compression, channels, sample_rate,
                 bits_per_sample, flags):
        self.compression = compression
        self.channels = channels
        self.sample_rate = sample_rate
        self.bits_per_sample = bits_per_sample
        self.flags = flags

    @property
    def codec_name(self):
        return COMPRESSION_NAMES.get(self.compression,
                                     f'Unknown(0x{self.compression:04X})')

    @property
    def bit_rate(self):
        return self.bits_per_sample * self.sample_rate * self.channels

    @property
    def block_align(self):
        return 36 * self.channels


class XMVVideoFrame:
    __slots__ = ('offset', 'size', 'timestamp', 'is_keyframe', 'data')

    def __init__(self, offset, size, timestamp, is_keyframe, data=None):
        self.offset = offset
        self.size = size
        self.timestamp = timestamp
        self.is_keyframe = is_keyframe
        self.data = data


class XMVPacket:
    __slots__ = ('offset', 'size', 'next_size', 'video_data_size',
                 'frame_count', 'has_extradata', 'extradata',
                 'audio_data_sizes', 'video_offset', 'audio_offsets',
                 'frames')

    def __init__(self):
        self.offset = 0
        self.size = 0
        self.next_size = 0
        self.video_data_size = 0
        self.frame_count = 0
        self.has_extradata = False
        self.extradata = None
        self.audio_data_sizes = []
        self.video_offset = 0
        self.audio_offsets = []
        self.frames = []


class XMVFile:
    """Parser for Xbox XMV video files."""

    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()

        if len(self.data) < 36:
            raise ValueError(f'File too small for XMV header ({len(self.data)} bytes)')
        if self.data[12:16] != b'xobX':
            raise ValueError(f'Not an XMV file (magic: {self.data[12:16]!r})')

        self._parse_header()
        self._parse_packets()

    def _parse_header(self):
        d = self.data
        self.next_packet_size = struct.unpack_from('<I', d, 0)[0]
        self.this_packet_size = struct.unpack_from('<I', d, 4)[0]
        self.max_packet_size = struct.unpack_from('<I', d, 8)[0]
        self.version = struct.unpack_from('<I', d, 16)[0]
        self.width = struct.unpack_from('<I', d, 20)[0]
        self.height = struct.unpack_from('<I', d, 24)[0]
        self.duration_ms = struct.unpack_from('<I', d, 28)[0]
        self.audio_track_count = struct.unpack_from('<H', d, 32)[0]

        self.audio_tracks = []
        off = 36
        for _ in range(self.audio_track_count):
            comp = struct.unpack_from('<H', d, off)[0]
            ch = struct.unpack_from('<H', d, off + 2)[0]
            sr = struct.unpack_from('<I', d, off + 4)[0]
            bps = struct.unpack_from('<H', d, off + 8)[0]
            flags = struct.unpack_from('<H', d, off + 10)[0]
            self.audio_tracks.append(XMVAudioTrack(comp, ch, sr, bps, flags))
            off += 12

        self.header_size = off
        self.first_packet_offset = off

    def _parse_packets(self):
        """Parse all packets in the file."""
        self.packets = []
        offset = self.first_packet_offset
        next_size = self.this_packet_size - self.first_packet_offset

        while offset < len(self.data) and next_size > 0:
            pkt = self._parse_packet(offset, next_size)
            if pkt is None:
                break
            self.packets.append(pkt)
            offset = pkt.offset + pkt.size
            next_size = pkt.next_size

    def _parse_packet(self, offset, size):
        """Parse a single packet at the given offset."""
        d = self.data
        min_size = 12 + self.audio_track_count * 4
        if offset + min_size > len(d):
            return None

        pkt = XMVPacket()
        pkt.offset = offset
        pkt.size = size

        pkt.next_size = struct.unpack_from('<I', d, offset)[0]

        # Video header (8 bytes)
        vid_word = struct.unpack_from('<I', d, offset + 4)[0]
        pkt.video_data_size = vid_word & 0x007FFFFF
        pkt.frame_count = (vid_word >> 23) & 0xFF
        pkt.has_extradata = bool(vid_word & 0x80000000)

        if pkt.frame_count == 0:
            pkt.frame_count = 1

        # Subtract 4 bytes per audio track from video (alignment quirk)
        pkt.video_data_size -= self.audio_track_count * 4

        # Audio headers (4 bytes each)
        hdr_off = offset + 12
        pkt.audio_data_sizes = []
        for i in range(self.audio_track_count):
            aud_word = struct.unpack_from('<I', d, hdr_off)[0]
            aud_size = aud_word & 0x007FFFFF
            if aud_size == 0 and i > 0:
                aud_size = pkt.audio_data_sizes[i - 1]
            pkt.audio_data_sizes.append(aud_size)
            hdr_off += 4

        # Data offsets
        data_start = hdr_off
        pkt.video_offset = data_start
        data_pos = data_start + pkt.video_data_size
        pkt.audio_offsets = []
        for aud_size in pkt.audio_data_sizes:
            pkt.audio_offsets.append(data_pos)
            data_pos += aud_size

        # Parse extradata if present
        if pkt.has_extradata and pkt.video_data_size >= 4:
            ed_raw = struct.unpack_from('<I', d, pkt.video_offset)[0]
            # Convert XMV extradata to standard WMV2 extradata layout
            mspel = (ed_raw >> 0) & 1
            loop_filter = (ed_raw >> 1) & 1
            abt = (ed_raw >> 2) & 1
            j_type = (ed_raw >> 3) & 1
            top_left_mv = (ed_raw >> 4) & 1
            per_mb_rl = (ed_raw >> 5) & 1
            slice_count = (ed_raw >> 6) & 7
            pkt.extradata = struct.pack('>I',
                (mspel << 15) | (loop_filter << 14) | (abt << 13) |
                (j_type << 12) | (top_left_mv << 11) | (per_mb_rl << 10) |
                (slice_count << 7))
            pkt.video_offset += 4
            pkt.video_data_size -= 4

        # Parse individual video frames within this packet
        pkt.frames = []
        frame_off = pkt.video_offset
        pts = 0
        remaining = pkt.video_data_size
        for _ in range(pkt.frame_count):
            if frame_off + 4 > len(d) or remaining < 4:
                break
            fhdr = struct.unpack_from('<I', d, frame_off)[0]
            frame_size = (fhdr & 0x1FFFF) * 4 + 4
            frame_ts = fhdr >> 17

            if frame_size + 4 > remaining:
                break

            frame_data_off = frame_off + 4
            frame_data = d[frame_data_off:frame_data_off + frame_size]

            # Check keyframe: bit 7 of first byte = 0 means keyframe
            is_key = len(frame_data) > 0 and not (frame_data[0] & 0x80)

            pkt.frames.append(XMVVideoFrame(
                offset=frame_data_off, size=frame_size,
                timestamp=pts, is_keyframe=is_key, data=frame_data))

            pts += frame_ts
            frame_off += frame_size + 4
            remaining -= frame_size + 4

        return pkt

    @property
    def total_frames(self):
        return sum(len(p.frames) for p in self.packets)

    @property
    def total_video_bytes(self):
        return sum(p.video_data_size for p in self.packets)

    @property
    def total_audio_bytes(self):
        return [sum(p.audio_data_sizes[i] for p in self.packets)
                for i in range(self.audio_track_count)]


def xmv_info(path):
    """Print detailed information about an XMV file."""
    xmv = XMVFile(path)

    print(f'File: {xmv.path.name}')
    print(f'Size: {len(xmv.data):,} bytes ({len(xmv.data)/1024:.1f} KB)')
    print(f'Version: {xmv.version}')
    print(f'Video: {xmv.width}x{xmv.height} WMV2, {xmv.duration_ms}ms '
          f'({xmv.duration_ms/1000:.2f}s)')
    print(f'Audio tracks: {xmv.audio_track_count}')

    for i, track in enumerate(xmv.audio_tracks):
        print(f'  Track {i}: {track.codec_name}, {track.channels}ch, '
              f'{track.sample_rate}Hz, {track.bits_per_sample}bps, '
              f'flags=0x{track.flags:04X}')

    print(f'\nPackets: {len(xmv.packets)}')
    print(f'Total video frames: {xmv.total_frames}')
    print(f'Total video data: {xmv.total_video_bytes:,} bytes')

    audio_totals = xmv.total_audio_bytes
    for i, total in enumerate(audio_totals):
        print(f'Total audio[{i}] data: {total:,} bytes')

    if xmv.total_frames > 0:
        fps = xmv.total_frames / (xmv.duration_ms / 1000.0)
        print(f'Estimated FPS: {fps:.1f}')

    # Per-packet breakdown
    print(f'\nPacket details:')
    for i, pkt in enumerate(xmv.packets):
        kf = sum(1 for f in pkt.frames if f.is_keyframe)
        print(f'  [{i:3d}] offset=0x{pkt.offset:08X} size={pkt.size:6d} '
              f'frames={pkt.frame_count:2d} (keyframes={kf}) '
              f'video={pkt.video_data_size:6d} '
              f'audio={pkt.audio_data_sizes}')
        if i >= 20 and len(xmv.packets) > 25:
            print(f'  ... ({len(xmv.packets) - 21} more packets)')
            break


def xmv_extract(path, output_dir='.'):
    """Extract raw video and audio streams from an XMV file."""
    xmv = XMVFile(path)
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    stem = xmv.path.stem

    # Extract raw WMV2 video frames (concatenated, LE bitstream)
    video_path = out / f'{stem}_video.wmv2raw'
    total_video = 0
    with open(video_path, 'wb') as f:
        for pkt in xmv.packets:
            for frame in pkt.frames:
                if frame.data:
                    f.write(frame.data)
                    total_video += len(frame.data)
    print(f'Video: {video_path} ({total_video:,} bytes, '
          f'{xmv.total_frames} frames)')

    # Extract audio for each track
    for ti in range(xmv.audio_track_count):
        track = xmv.audio_tracks[ti]
        ext = 'adpcm' if track.compression == 0x0069 else 'wma'
        audio_path = out / f'{stem}_audio{ti}.{ext}'
        total_audio = 0
        with open(audio_path, 'wb') as f:
            for pkt in xmv.packets:
                if ti < len(pkt.audio_offsets):
                    aoff = pkt.audio_offsets[ti]
                    asize = pkt.audio_data_sizes[ti]
                    f.write(xmv.data[aoff:aoff + asize])
                    total_audio += asize
        print(f'Audio[{ti}]: {audio_path} ({total_audio:,} bytes, '
              f'{track.codec_name} {track.channels}ch {track.sample_rate}Hz)')

    # Write frame index
    idx_path = out / f'{stem}_frames.txt'
    with open(idx_path, 'w') as f:
        f.write(f'# {xmv.path.name} frame index\n')
        f.write(f'# {xmv.width}x{xmv.height} {xmv.duration_ms}ms '
                f'{xmv.total_frames} frames\n')
        f.write(f'# packet frame offset size timestamp keyframe\n')
        for pi, pkt in enumerate(xmv.packets):
            for fi, frame in enumerate(pkt.frames):
                f.write(f'{pi:4d} {fi:3d} 0x{frame.offset:08X} '
                        f'{frame.size:6d} {frame.timestamp:6d} '
                        f'{"K" if frame.is_keyframe else " "}\n')
    print(f'Index: {idx_path}')


def xmv_convert(path, output=None, ffmpeg='ffmpeg'):
    """Convert an XMV file to MP4 using FFmpeg.

    FFmpeg has native XMV demuxer support, so we just call it directly.
    The WMV2 video is transcoded to H.264 and audio to AAC.
    """
    path = Path(path)
    if output is None:
        output = path.with_suffix('.mp4')
    else:
        output = Path(output)

    # Verify the file is valid XMV first
    xmv = XMVFile(path)
    print(f'Converting: {path.name} ({xmv.width}x{xmv.height}, '
          f'{xmv.duration_ms/1000:.1f}s, {xmv.total_frames} frames)')

    cmd = [
        ffmpeg, '-y', '-i', str(path),
        '-c:v', 'libx264', '-preset', 'fast', '-crf', '18',
        '-c:a', 'aac', '-b:a', '128k',
        '-movflags', '+faststart',
        str(output)
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode == 0:
            out_size = output.stat().st_size
            print(f'  -> {output.name} ({out_size:,} bytes)')
            return True
        else:
            print(f'  FFmpeg error (code {result.returncode}):')
            # Show last few lines of stderr
            for line in result.stderr.strip().split('\n')[-5:]:
                print(f'    {line}')
            return False
    except FileNotFoundError:
        print(f'  Error: FFmpeg not found at "{ffmpeg}"')
        print(f'  Install FFmpeg or pass --ffmpeg <path>')
        return False
    except subprocess.TimeoutExpired:
        print(f'  Error: FFmpeg timed out after 120s')
        return False


def xmv_batch(directory, output_dir=None, ffmpeg='ffmpeg'):
    """Convert all XMV files in a directory to MP4."""
    src = Path(directory)
    if not src.is_dir():
        print(f'Error: {src} is not a directory')
        return

    xmv_files = sorted(src.glob('*.xmv')) + sorted(src.glob('*.XMV'))
    if not xmv_files:
        print(f'No XMV files found in {src}')
        return

    dst = Path(output_dir) if output_dir else src
    dst.mkdir(parents=True, exist_ok=True)

    print(f'Converting {len(xmv_files)} XMV files...\n')
    ok = 0
    fail = 0
    for xmv_path in xmv_files:
        out_path = dst / xmv_path.with_suffix('.mp4').name
        if xmv_convert(xmv_path, out_path, ffmpeg):
            ok += 1
        else:
            fail += 1

    print(f'\nDone: {ok} converted, {fail} failed')
