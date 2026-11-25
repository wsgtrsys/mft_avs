//----------------------------------------------------------------------------------------------
// classGuids.h - CLSID of the AVS/AVS2/AVS3 video decoder MFT plus the media sub-types
//                it accepts as input.
//
// Media Foundation has no built-in sub-type for AVS bit streams (the MPEG-2 TS byte
// stream handler that ships with Windows does not even know stream_type 0x42 = AVS),
// so the decoder defines its own fourCC-based sub-types, exactly the way
// MFVideoFormat_H264 / MFVideoFormat_NV12 are defined.  A custom byte stream handler
// or media source that feeds this decoder must use these GUIDs.
//----------------------------------------------------------------------------------------------
#ifndef CLASSGUIDS_H
#define CLASSGUIDS_H

// {9E5D6A1B-2C4F-4E7D-8F2B-7A9C0D5E3B11}
DEFINE_GUID(CLSID_AVSVideoDecoder,
    0x9e5d6a1b, 0x2c4f, 0x4e7d, 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x11);

// {9E5D6A1B-2C4F-4E7D-8F2B-7A9C0D5E3B12}
DEFINE_GUID(CLSID_AVSAudioDecoder,
    0x9e5d6a1b, 0x2c4f, 0x4e7d, 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x12);

// {9E5D6A1B-2C4F-4E7D-8F2B-7A9C0D5E3B13}
DEFINE_GUID(CLSID_AVSMediaSource,
    0x9e5d6a1b, 0x2c4f, 0x4e7d, 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x13);

// {9E5D6A1B-2C4F-4E7D-8F2B-7A9C0D5E3B14}
DEFINE_GUID(CLSID_AVSByteStreamHandler,
    0x9e5d6a1b, 0x2c4f, 0x4e7d, 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x14);

// interface of the media source, lets a caller hand us its bytes directly
// {7B8E2A4C-1D3F-4A6B-9C8D-2E4F6A7B8C9D}
DEFINE_GUID(IID_IAVSByteDataSource,
    0x7b8e2a4c, 0x1d3f, 0x4a6b, 0x9c, 0x8d, 0x2e, 0x4f, 0x6a, 0x7b, 0x8c, 0x9d);

// string attribute on the media source: the FFmpeg demuxer name to use ("mpegts",
// "cavsvideo", ...).  Set by the byte stream handler from the file extension, because
// a raw AVS elementary stream cannot be recognized by content probing.
// {A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C5D}
DEFINE_GUID(AVS_SOURCE_ATTRIBUTE_DEMUX_HINT,
    0xa1b2c3d4, 0x5e6f, 0x4a7b, 0x8c, 0x9d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d);

// DRA (AVS audio, GB/T 20090.6) elementary stream, fourCC 'DRA '
// {20415244-0000-0010-8000-00AA00389B71}
DEFINE_GUID(MFAudioFormat_DRA,
    0x20415244, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// AVS1-P2 / AVS1-P16 (AVS+) elementary stream, fourCC 'AVS '
// {20535641-0000-0010-8000-00AA00389B71}
DEFINE_GUID(MFVideoFormat_AVS,
    0x20535641, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// AVS2-P2 / AVS2-P16 (IEEE 1857.4) elementary stream, fourCC 'AVS2'
// {32535641-0000-0010-8000-00AA00389B71}
DEFINE_GUID(MFVideoFormat_AVS2,
    0x32535641, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// AVS3-P3 (IEEE 1857.6) elementary stream, fourCC 'AVS3'
// {33535641-0000-0010-8000-00AA00389B71}
DEFINE_GUID(MFVideoFormat_AVS3,
    0x33535641, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

// Measurement only (plan/avs-mf-playback.md KR7.2 candidate A): the same AVS+ elementary
// stream described by a GUID that is NOT in Media Foundation's FourCC layout
// {fourcc-0000-0010-8000-00AA00389B71}.  All three AVS subtypes use that layout, and so do
// 96 GUIDs in this Windows SDK (MEDIASUBTYPE_AVC1, MEDIASUBTYPE_MPEG_ADTS_AAC, ...), which is
// why the layout is not expected to matter - this measures it instead of arguing about it.
// {88D5E9F7-9775-4062-88DF-536A56166781}
DEFINE_GUID(MFVideoFormat_AVS_ALT,
    0x88d5e9f7, 0x9775, 0x4062, 0x88, 0xdf, 0x53, 0x6a, 0x56, 0x16, 0x67, 0x81);

#endif // CLASSGUIDS_H
