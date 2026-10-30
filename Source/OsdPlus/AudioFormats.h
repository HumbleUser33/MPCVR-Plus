// Bounded parsing of audio connection types already supplied by DirectShow.
// GUID/format facts: Windows SDK, MPC-BE and LAVFilters (see patch notes).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "AudioDepth.h"

namespace OsdAudio {
struct Guid {
    std::uint32_t Data1;
    std::uint16_t Data2, Data3;
    std::uint8_t Data4[8];
    constexpr bool operator==(const Guid& rhs) const {
        if (Data1 != rhs.Data1 || Data2 != rhs.Data2 || Data3 != rhs.Data3) return false;
        for (unsigned i = 0; i < 8; ++i) if (Data4[i] != rhs.Data4[i]) return false;
        return true;
    }
};
template<class T> inline Guid FromGuid(const T& value) {
    return {value.Data1, value.Data2, value.Data3,
        {value.Data4[0], value.Data4[1], value.Data4[2], value.Data4[3],
         value.Data4[4], value.Data4[5], value.Data4[6], value.Data4[7]}};
}
constexpr Guid WaveGuid(std::uint32_t id) { return {id, 0, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}}; }
constexpr Guid IecGuid(std::uint32_t id) { return {id, 0x0cea, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}}; }
constexpr Guid DvdGuid(std::uint32_t id) { return {id, 0xdb46, 0x11cf, {0xb4,0xd1,0,0x80,0x5f,0x6c,0xbb,0xea}}; }
inline constexpr Guid WaveFormat = {0x05589f81,0xc356,0x11ce,{0xbf,1,0,0xaa,0,0x55,0x59,0x5a}};
inline constexpr Guid VorbisFormat = {0x6bddfa7e,0x9f22,0x46a9,{0xab,0x5e,0x88,0x4e,0xff,0x29,0x4d,0x9f}};
inline constexpr Guid VorbisFormat2 = {0xb36e107f,0xa938,0x4387,{0x93,0xc7,0x55,0xe9,0x66,0x75,0x74,0x73}};
inline constexpr Guid DdPlus = {0xa7fb87af,0x2d02,0x42fb,{0xa4,0xd4,5,0xcd,0x93,0x84,0x3b,0xdd}};
inline constexpr Guid TrueHd = {0xeb27cec4,0x163e,0x4ca3,{0x8b,0x74,0x8e,0x25,0xf9,0x1b,0x51,0x7e}};
inline constexpr Guid DtsHd = {0xa2e58eb7,0x0fa9,0x48bb,{0xa4,0x0c,0xfa,0x0e,0x15,0x6d,6,0x45}};
inline constexpr Guid DtsHdHra = {0xa61ac364,0xad0e,0x4744,{0x89,0xff,0x21,0x3c,0xe0,0xdf,0x88,4}};
inline constexpr Guid DvdLpcm = DvdGuid(0xe06d8032);
inline constexpr Guid HdmvLpcm = {0x949f97fd,0x56f6,0x4527,{0xb4,0xae,0xdd,0xeb,0x37,0x5a,0xb8,0x0f}};
inline constexpr Guid BdLpcm = {0xa23eb7fc,0x510b,0x466f,{0x9f,0xbf,0x5f,0x87,0x8f,0x69,0x34,0x7c}};
inline constexpr Guid FlacFramed = {0x1541c5c0,0xcddf,0x477d,{0xbc,0x0a,0x86,0xf8,0xae,0x7f,0x83,0x54}};
inline constexpr Guid Vorbis = {0xcddca2d5,0x6d75,0x4f98,{0x84,0x0e,0x73,0x7b,0xed,0xd5,0xc6,0x3b}};
inline constexpr Guid Vorbis2 = {0x8d2fd10b,0x5841,0x4a6b,{0x89,5,0x58,0x8f,0xec,0x1a,0xde,0xd9}};

enum class Family { Other, LinearPcm, Flac };
struct Codec { std::wstring_view Name; Family Type = Family::Other; };
inline Codec WaveCodec(std::uint32_t id) {
    switch (id) {
    case 1: return {L"PCM", Family::LinearPcm};
    case 3: return {L"Float PCM", Family::LinearPcm};
    case 2: return {L"MS ADPCM"};
    case 6: return {L"A-law"};
    case 7: return {L"mu-law"};
    case 8: case 0x2001: return {L"DTS"};
    case 0x11: return {L"IMA ADPCM"};
    case 0x31: return {L"GSM 6.10"};
    case 0x50: return {L"MPEG Audio"};
    case 0x55: return {L"MP3"};
    case 0x92: return {L"AC-3 S/PDIF"};
    case 0xfe: return {L"AMR"};
    case 0xff: case 0x1600: case 0x1610: return {L"AAC"};
    case 0x1ff: return {L"AAC (LATM)"};
    case 0x160: return {L"WMA 1"};
    case 0x161: return {L"WMA 2"};
    case 0x162: return {L"WMA Pro"};
    case 0x163: return {L"WMA Lossless"};
    case 0x270: return {L"ATRAC3"};
    case 0x2000: return {L"AC-3"};
    case 0xf1ac: return {L"FLAC", Family::Flac};
    case 0x704f: case 0x5355504f: return {L"Opus"};
    case 0x5756: return {L"WavPack"};
    case 0x77a1: return {L"TTA"};
    case 0xa109: return {L"Speex"};
    case 0x674f: case 0x6750: case 0x6751: case 0x676f: case 0x6770: case 0x6771: return {L"Vorbis"};
    case 0x4134504d: case 0x6134706d: return {L"MPEG-4 Audio"};
    case 0x53544441: return {L"AAC (ADTS)"};
    case 0x63616c61: return {L"ALAC"};
    case 0x20455041: return {L"APE"};
    case 0x20504c4d: return {L"MLP"};
    case 0x20534c41: return {L"ALS"};
    case 0x454e4f4e: case 0x20776172: case 0x736f7774: case 0x74776f73:
    case 0x34326e69: case 0x32336e69: return {L"QuickTime PCM"};
    case 0x32336c66: case 0x34366c66: return {L"QuickTime Float PCM"};
    case 0x34616d69: return {L"IMA4 ADPCM"};
    case 0x3843504d: return {L"Musepack SV8"};
    case 0x2043504d: return {L"Musepack SV7"};
    case 0x6e726873: return {L"Shorten"};
    case 0x726d6173: return {L"AMR-NB"};
    case 0x62776173: return {L"AMR-WB"};
    case 0x434d4451: return {L"QDesign Music"};
    case 0x324d4451: return {L"QDesign Music 2"};
    case 0x20445344: case 0x4c445344: case 0x4d445344: case 0x31445344: case 0x38445344: return {L"DSD"};
    case 0x20545344: return {L"DST"};
    case 0x33534541: return {L"AES3"};
    case 0x342d4341: return {L"AC-4"};
    default: return {};
    }
}
inline Codec GuidCodec(const Guid& id) {
    // Compare all 128 bits before using a GUID's numeric tag/FourCC.
    if (id == WaveGuid(id.Data1)) return WaveCodec(id.Data1);
    struct Entry { Guid Id; Codec Value; };
    static constexpr Entry entries[] = {
        {DdPlus, {L"E-AC-3 (Dolby Digital Plus)"}}, {TrueHd, {L"Dolby TrueHD"}},
        {DtsHd, {L"DTS-HD"}}, {DtsHdHra, {L"DTS-HD High Resolution"}},
        {DvdGuid(0xe06d802b), {L"MPEG Audio"}}, {DvdGuid(0xe06d802c), {L"AC-3"}},
        {DvdGuid(0xe06d8033), {L"DTS"}}, {DvdGuid(0xe06d8034), {L"SDDS"}},
        {DvdLpcm, {L"DVD LPCM"}}, {HdmvLpcm, {L"Blu-ray LPCM"}}, {BdLpcm, {L"Blu-ray LPCM"}},
        {IecGuid(0x0a), {L"E-AC-3 (IEC 61937)"}}, {IecGuid(0x0b), {L"DTS-HD (IEC 61937)"}},
        {IecGuid(0x0c), {L"MLP/TrueHD (IEC 61937)"}}, {IecGuid(0x10a), {L"E-AC-3 + Dolby Atmos (IEC 61937)"}},
        {FlacFramed, {L"FLAC", Family::Flac}}, {Vorbis, {L"Vorbis"}}, {Vorbis2, {L"Vorbis"}},
        {{0x71cfa727,0x37e4,0x404a,{0xae,0xc0,0x34,0x84,0x25,0x32,0xef,0xf7}}, {L"E-AC-3 (Dolby Digital Plus)"}},
        {{0x4288b843,0x610b,0x4e15,{0xa5,0x3b,0x43,0,0x7f,0xcf,0xf6,0x14}}, {L"Dolby TrueHD"}},
        {{0xb82196e9,0x1b3f,0x4796,{0xa6,0x36,0x46,0x23,0x90,0x87,0xb3,0x8e}}, {L"TAK"}},
        {{0x20c518c0,0xc136,0x4fb8,{0x84,0x18,2,0x49,0x34,0xf0,0xba,0xca}}, {L"TAK"}},
        {{0xff5a7c6f,0x6646,0x42ab,{0xa1,0xfc,0x0e,0x67,0x39,0xd5,0x6e,0x75}}, {L"WavPack"}},
        {{0xb8cbbad8,0xaa1a,0x4cea,{0xb9,0x5e,0x73,0,0x41,0xa5,0x5e,0xf0}}, {L"WavPack Hybrid"}},
        {{0xa721ec57,0x6eeb,0x47a3,{0x88,5,0xad,0x75,0xe0,0x41,0x7c,0x26}}, {L"TTA"}},
        {{0x229f9ad7,0x2f4d,0x4b45,{0xb8,0x58,0xa3,0xf4,0x34,0xe7,0x17,0x9a}}, {L"Musepack"}},
        {{0xe923aabf,0xcb58,0x4471,{0xa1,0x19,0xff,0xfa,1,0xe4,0xce,0x62}}, {L"ATRAC3plus"}},
        {{0x47e142d2,0x36ba,0x4d8d,{0x88,0xfc,0x61,0x65,0x4f,0x8c,0x83,0x6c}}, {L"ATRAC9"}},
    };
    for (const auto& entry : entries) if (id == entry.Id) return entry.Value;
    return {};
}

inline std::uint16_t U16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8));
}
inline std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline Guid ReadGuid(const std::uint8_t* p) {
    return {U32(p), U16(p+4), U16(p+6), {p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]}};
}
inline unsigned FlacDepth(const std::uint8_t* p, std::size_t n, unsigned rate, unsigned channels) {
    bool marked = false;
    if (n >= 4 && p[0]=='f' && p[1]=='L' && p[2]=='a' && p[3]=='C') { p+=4; n-=4; marked=true; }
    if (n >= 38 && (p[0]&0x7f)==0 && p[1]==0 && p[2]==0 && p[3]==34) { p+=4; n=34; }
    else if (marked || n!=34) return 0;
    if (n!=34) return 0;
    const unsigned minBlock=(unsigned(p[0])<<8)|p[1], maxBlock=(unsigned(p[2])<<8)|p[3];
    const unsigned sampleRate=(unsigned(p[10])<<12)|(unsigned(p[11])<<4)|(p[12]>>4);
    const unsigned count=((p[12]>>1)&7)+1, depth=(((unsigned(p[12])&1)<<4)|(p[13]>>4))+1;
    return minBlock>=16 && maxBlock>=minBlock && depth>=4 && depth<=32 && sampleRate!=0
        && sampleRate==rate && count==channels ? depth : 0;
}
struct Details {
    Codec Encoding;
    Guid Identity = {};
    unsigned Tag=0, Rate=0, Channels=0, Bits=0;
    std::uint64_t BitRate=0;
    bool LinearPcm=false;
};
inline bool MpcBeProfileSource(Guid id) {
    // CLSIDs of the producers whose one-byte private format has been checked.
    static constexpr Guid sources[] = {
        {0x149d2e01,0xc32e,0x4939,{0x80,0xf6,0xc0,0x7b,0x81,1,0x5a,0x7a}}, // Matroska splitter
        {0x0a68c3b5,0x9164,0x4a54,{0xaf,0xaf,0x99,0x5b,0x2f,0xf0,0xe0,0xd4}}, // Matroska source
        {0x61f47056,0xe400,0x43d3,{0xaf,0x1e,0xab,0x7d,0xff,0xd4,0xc4,0xad}}, // MP4 splitter
        {0x3ccc052e,0xbdee,0x408a,{0xbe,0xa7,0x90,0x91,0x4e,0xf2,0x96,0x4b}}, // MP4 source
        {0xdc257063,0x045f,0x4be2,{0xbd,0x5b,0xe1,0x22,0x79,0xc4,0x64,0xf0}}, // MPEG splitter
        {0x1365be7a,0xc86a,0x473c,{0x9a,0x41,0xc0,0xa6,0xe8,0x2c,0x9f,0xa3}}, // MPEG source
    };
    for (const auto& source : sources) if (id==source) return true;
    return false;
}
inline std::wstring_view MpcBeDtsProfile(unsigned profile) {
    switch (profile) {
    case 1: return L"DTS-HD High Resolution";
    case 2: return L"DTS-HD Master Audio";
    case 3: return L"DTS Express";
    case 4: return L"DTS-HD Master Audio + DTS:X";
    case 5: return L"DTS-HD Master Audio + DTS:X IMAX";
    case 6: return L"DTS-HD High Resolution + DTS:X";
    case 7: return L"DTS-HD High Resolution + DTS:X IMAX";
    default: return {};
    }
}
inline Details Parse(Guid subtype, Guid format, const void* data, std::size_t size,
                     std::uint32_t current=0, bool mpcBeProfile=false) {
    Details out;
    out.Identity=subtype; out.Encoding=GuidCodec(subtype); out.BitRate=current;
    if (!data) return out;
    const auto* p=static_cast<const std::uint8_t*>(data);
    if (format==WaveFormat) {
        // PCM's optional cbSize word may be omitted (16-byte PCMWAVEFORMAT).
        if (size<16 || (size<18 && U16(p)!=1 && U16(p)!=3)) return out;
        out.Tag=U16(p); out.Channels=U16(p+2); out.Rate=U32(p+4); out.Bits=U16(p+14);
        const std::size_t extra=size>=18 ? U16(p+16) : 0;
        const bool boundedExtra=size>=18 && extra<=size-18;
        if (out.Tag==0xfffe && boundedExtra && extra>=22 && size>=40) {
            const Guid inner=ReadGuid(p+24);
            if (out.Encoding.Name.empty()) { out.Identity=inner; out.Encoding=GuidCodec(inner); }
            const unsigned valid=U16(p+18);
            if (valid && valid<=out.Bits) out.Bits=valid;
        }
        // An extensible tag is not itself a codec; never truncate SubFormat.Data1.
        if (out.Encoding.Name.empty() && out.Tag!=0xfffe) out.Encoding=WaveCodec(out.Tag);
        if (!out.Bits && out.Encoding.Type==Family::Flac && boundedExtra)
            out.Bits=FlacDepth(p+18,extra,out.Rate,out.Channels);
        // MPC-BE carries these facts in an exact one-byte private extension.
        // Only interpret it when the caller has verified the producing splitter.
        if (mpcBeProfile && boundedExtra && extra==1 && size==19) {
            if (out.Encoding.Name==L"DTS") {
                const auto name=MpcBeDtsProfile(p[18]);
                if (!name.empty()) out.Encoding.Name=name;
            } else if (p[18]==1 && subtype==TrueHd) {
                out.Encoding.Name=L"Dolby TrueHD + Dolby Atmos";
            } else if (p[18]==1 && subtype==DdPlus) {
                out.Encoding.Name=L"E-AC-3 (Dolby Digital Plus) + Dolby Atmos";
            }
        }
        if (!current) out.BitRate=std::uint64_t(U32(p+8))*8;
        out.LinearPcm=out.Encoding.Type==Family::LinearPcm;
    } else if (format==VorbisFormat || format==VorbisFormat2) {
        if (out.Encoding.Name.empty()) out.Encoding={L"Vorbis"};
        if (out.Encoding.Name!=L"Vorbis" || size<24) return out;
        if (format==VorbisFormat) {
            // VORBISFORMAT uses Windows default alignment (WORD, then DWORD).
            out.Channels=U16(p); out.Rate=U32(p+4);
            if (!current) out.BitRate=U32(p+12); // Already bits/s, not bytes/s.
        } else {
            out.Channels=U32(p); out.Rate=U32(p+4); out.Bits=U32(p+8);
            const std::uint64_t h0=U32(p+12), h1=U32(p+16), h2=U32(p+20);
            if (!current && h0>=30 && h0+h1+h2<=size-24) {
                const auto* h=p+24;
                const unsigned blockSmall=h[28]&15, blockLarge=h[28]>>4;
                if (h[0]==1 && h[1]=='v' && h[2]=='o' && h[3]=='r' && h[4]=='b' && h[5]=='i' && h[6]=='s'
                    && U32(h+7)==0 && h[11]==out.Channels && U32(h+12)==out.Rate && out.Rate
                    && blockSmall>=6 && blockLarge>=blockSmall && blockLarge<=13 && (h[29]&1)) {
                    const std::uint32_t nominal=U32(h+20);
                    if (nominal && nominal<0x80000000u) out.BitRate=nominal;
                }
            }
        }
    }
    out.Bits=OsdAudioDepth(out.Tag,out.Bits);
    return out;
}
} // namespace OsdAudio
