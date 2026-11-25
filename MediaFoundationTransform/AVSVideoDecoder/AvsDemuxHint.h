// ****************************************************************************
// AvsDemuxHint.h
//
// Which libavformat demuxer a file name implies.
//
// Raw AVS elementary streams cannot be recognised from their content alone: the
// generations share the 000001xx start code space, and the raw demuxers of one
// generation will happily score the bit stream of another.  The extension is the
// only reliable hint, and it selects the demuxer that also sets the right codec
// id (cavsvideo -> CAVS, avs2 -> AVS2, avs3 -> AVS3), which in turn decides the
// Media Foundation sub type published for the stream.
//
// Shared by CAVSByteStreamHandler (the URL path) and by the test tools, which
// create the media source directly and have to pass the same hint attribute.
// ****************************************************************************
#pragma once

#include <string>
#include <cstring>

// "cavsvideo" / "avs2" / "avs3" / "mpegts", or empty when the extension says
// nothing about the container.
inline std::string AvsDemuxHintForExt(const std::string& extIn)   // ".avs" ...
{
    std::string ext = extIn;
    for (size_t i = 0; i < ext.size(); i++)
        if (ext[i] >= 'A' && ext[i] <= 'Z')
            ext[i] = (char)(ext[i] - 'A' + 'a');

    if (ext == ".ts" || ext == ".m2ts" || ext == ".mpts" || ext == ".mpg" || ext == ".mpeg")
        return "mpegts";
    if (ext == ".avs" || ext == ".cavs")
        return "cavsvideo";
    if (ext == ".avs2")
        return "avs2";
    if (ext == ".avs3")
        return "avs3";
    return std::string();
}

// The raw elementary stream demuxers of the AVS family.  A content probe cannot
// tell them apart, so one member of the family must never override another: the
// extension decides.
inline bool AvsIsRawAvsDemuxer(const char* name)
{
    return name != NULL &&
           (strcmp(name, "cavsvideo") == 0 || strcmp(name, "avs2") == 0 ||
            strcmp(name, "avs3") == 0);
}

// The extensions this DLL registers for itself.  DllMain.cpp kDefaultExtensions is the
// registry side of this same list: if the two drift apart, files named .avs that carry
// standard codecs stop being claimed and our own measurement controls (11d, 11i) go red.
// ".ts" and ".m2ts" are deliberately NOT here even when AVS_MFT_REGISTER_TS adds them: a
// broadcast container holding only standard codecs has to go back to the system handler.
inline bool AvsOwnExtension(const std::string& extIn)
{
    std::string ext = extIn;
    for (size_t i = 0; i < ext.size(); i++)
        if (ext[i] >= 'A' && ext[i] <= 'Z')
            ext[i] = (char)(ext[i] - 'A' + 'a');
    return ext == ".avs" || ext == ".cavs" || ext == ".avs2" || ext == ".avs3";
}
