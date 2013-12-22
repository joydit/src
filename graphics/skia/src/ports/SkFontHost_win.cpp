/*
 ** Copyright 2006, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#define _WIN32_WINNT 0x0502
#include "SkString.h"
//#include "SkStream.h"

#include "SkEndian.h"
#include "SkFontHost.h"
#include "SkDescriptor.h"
#include "SkAdvancedTypefaceMetrics.h"
#include "SkStream.h"
#include "SkThread.h"
#include "SkTypeface_win.h"
#include "SkTypefaceCache.h"
#include "SkUtils.h"
#include "SkScalar.h"
#include "SkFixed.h"

#ifdef WIN32
#include "windows.h"
#include "tchar.h"
#include "Usp10.h"

static bool compute_bounds_outset(const LOGFONT& lf, SkIRect* outset) {

    static const struct {
        const char* fUCName;    // UTF8 encoded, ascii is upper-case
        SkIRect     fOutset;    // these are deltas for the glyph's bounds
    } gData[] = {
        // http://code.google.com/p/chromium/issues/detail?id=130842
        { "DOTUM", { 0, 0, 0, 1 } },
        { "DOTUMCHE", { 0, 0, 0, 1 } },
        { "\xEB\x8F\x8B\xEC\x9B\x80", { 0, 0, 0, 1 } },
        { "\xEB\x8F\x8B\xEC\x9B\x80\xEC\xB2\xB4", { 0, 0, 0, 1 } },
        { "MS UI GOTHIC", { 1, 0, 0, 0 } },
    };

    /**
     *  We convert the target name into upper-case (for ascii chars) UTF8.
     *  Our database is already stored in this fashion, and it allows us to
     *  search it with straight memcmp, since everyone is in this canonical
     *  form.
     */

    // temp storage is max # TCHARs * max expantion for UTF8 + null
    char name[kMaxBytesInUTF8Sequence * LF_FACESIZE + 1];
    int index = 0;
    for (int i = 0; i < LF_FACESIZE; ++i) {
        uint16_t c = lf.lfFaceName[i];
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }
        size_t n = SkUTF16_ToUTF8(&c, 1, &name[index]);
        index += n;
        if (0 == c) {
            break;
        }
    }

    for (size_t j = 0; j < SK_ARRAY_COUNT(gData); ++j) {
        if (!strcmp(gData[j].fUCName, name)) {
            *outset = gData[j].fOutset;
            return true;
        }
    }
    return false;
}

// client3d has to undefine this for now
#define CAN_USE_LOGFONT_NAME

using namespace skia_advanced_typeface_metrics_utils;

static const uint16_t BUFFERSIZE = (16384 - 32);
static uint8_t glyphbuf[BUFFERSIZE];

// Give 1MB font cache budget
#define FONT_CACHE_MEMORY_BUDGET    (1024 * 1024)

// outset isn't really a rect, but 4 (non-negative) values to outset the
// glyph's metrics by. For "normal" fonts, all these values should be 0.
static void apply_outset(SkGlyph* glyph, const SkIRect& outset) {
    SkASSERT(outset.fLeft >= 0);
    SkASSERT(outset.fTop >= 0);
    SkASSERT(outset.fRight >= 0);
    SkASSERT(outset.fBottom >= 0);

    glyph->fLeft -= outset.fLeft;
    glyph->fTop -= outset.fTop;
    glyph->fWidth += outset.fLeft + outset.fRight;
    glyph->fHeight += outset.fTop + outset.fBottom;
}

// always packed xxRRGGBB
typedef uint32_t SkGdiRGB;

template <typename T> T* SkTAddByteOffset(T* ptr, size_t byteOffset) {
    return (T*)((char*)ptr + byteOffset);
}

/**
 *	Since LOGFONT wants its textsize as an int, and we support fractional sizes,
 *  and since we have a cache of LOGFONTs for our tyepfaces, we always set the
 *  lfHeight to a canonical size, and then we use the 2x2 matrix to achieve the
 *  actual requested size.
 */
static const int gCanonicalTextSize = 64;

static void make_canonical(LOGFONT* lf) {
	lf->lfHeight = -gCanonicalTextSize;
    lf->lfQuality = CLEARTYPE_QUALITY;//PROOF_QUALITY;
    lf->lfCharSet = DEFAULT_CHARSET;
}

static SkTypeface::Style getStyle(const LOGFONT& lf) {
    unsigned style = 0;
    if (lf.lfWeight >= FW_BOLD) {
        style |= SkTypeface::kBold;
    }
    if (lf.lfItalic) {
        style |= SkTypeface::kItalic;
    }
    return (SkTypeface::Style)style;
}

static void setStyle(LOGFONT* lf, SkTypeface::Style style) {
    lf->lfWeight = (style & SkTypeface::kBold) != 0 ? FW_BOLD : FW_NORMAL ;
    lf->lfItalic = ((style & SkTypeface::kItalic) != 0);
}

static inline FIXED SkFixedToFIXED(SkFixed x) {
    return *(FIXED*)(&x);
}
static inline SkFixed SkFIXEDToFixed(FIXED x) {
    return *(SkFixed*)(&x);
}

static inline FIXED SkScalarToFIXED(SkScalar x) {
    return SkFixedToFIXED(SkScalarToFixed(x));
}

static unsigned calculateOutlineGlyphCount(HDC hdc) {
    // The 'maxp' table stores the number of glyphs at offset 4, in 2 bytes.
    const DWORD maxpTag =
        SkEndian_SwapBE32(SkSetFourByteTag('m', 'a', 'x', 'p'));
    uint16_t glyphs;
    if (GetFontData(hdc, maxpTag, 4, &glyphs, sizeof(glyphs)) != GDI_ERROR) {
        return SkEndian_SwapBE16(glyphs);
    }

    // Binary search for glyph count.
    static const MAT2 mat2 = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    int32_t max = SK_MaxU16 + 1;
    int32_t min = 0;
    GLYPHMETRICS gm;
    while (min < max) {
        int32_t mid = min + ((max - min) / 2);
        if (GetGlyphOutlineW(hdc, mid, GGO_METRICS | GGO_GLYPH_INDEX, &gm, 0,
            NULL, &mat2) == GDI_ERROR) {
                max = mid;
        } else {
            min = mid + 1;
        }
    }
    SkASSERT(min == max);
    return min;
}

static unsigned calculateGlyphCount(HDC hdc) {
    // The 'maxp' table stores the number of glyphs at offset 4, in 2 bytes.
    const DWORD maxpTag =
        SkEndian_SwapBE32(SkSetFourByteTag('m', 'a', 'x', 'p'));
    uint16_t glyphs;
    if (GetFontData(hdc, maxpTag, 4, &glyphs, sizeof(glyphs)) != GDI_ERROR) {
        return SkEndian_SwapBE16(glyphs);
    }
    
    // Binary search for glyph count.
    static const MAT2 mat2 = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    int32_t max = SK_MaxU16 + 1;
    int32_t min = 0;
    GLYPHMETRICS gm;
    while (min < max) {
        int32_t mid = min + ((max - min) / 2);
        if (GetGlyphOutlineW(hdc, mid, GGO_METRICS | GGO_GLYPH_INDEX, &gm, 0,
                             NULL, &mat2) == GDI_ERROR) {
            max = mid;
        } else {
            min = mid + 1;
        }
    }
    SkASSERT(min == max);
    return min;
}

static SkTypeface::Style GetFontStyle(const LOGFONT& lf) {
    int style = SkTypeface::kNormal;
    if (lf.lfWeight == FW_SEMIBOLD || lf.lfWeight == FW_DEMIBOLD || lf.lfWeight == FW_BOLD)
        style |= SkTypeface::kBold;
    if (lf.lfItalic)
        style |= SkTypeface::kItalic;

    return (SkTypeface::Style)style;
}

class LogFontTypeface : public SkTypeface {
public:
    LogFontTypeface(SkTypeface::Style style, SkFontID fontID, const LOGFONT& lf) :
      SkTypeface(style, fontID, false), fLogFont(lf) {}

    LOGFONT fLogFont;

    static LogFontTypeface* Create(const LOGFONT& lf) {
        SkTypeface::Style style = GetFontStyle(lf);
        SkFontID fontID = SkTypefaceCache::NewFontID();
        //return new LogFontTypeface(style, fontID, lf);
        return SkNEW_ARGS(LogFontTypeface, (style, fontID, lf)); // sk_weolar
    }
};

static int alignTo32(int n) {
    return (n + 31) & ~31;
}

struct MyBitmapInfo : public BITMAPINFO {
    RGBQUAD fMoreSpaceForColors[1];
};

// define this in your Makefile or .gyp to enforce AA requests
// which GDI ignores at small sizes. This flag guarantees AA
// for rotated text, regardless of GDI's notions.
//#define SK_ENFORCE_ROTATED_TEXT_AA_ON_WINDOWS

// client3d has to undefine this for now
#define CAN_USE_LOGFONT_NAME

static bool isLCD(const SkScalerContext::Rec& rec) {
    return SkMask::kLCD16_Format == rec.fMaskFormat ||
        SkMask::kLCD32_Format == rec.fMaskFormat;
}

static bool bothZero(SkScalar a, SkScalar b) {
    return 0 == a && 0 == b;
}

// returns false if there is any non-90-rotation or skew
static bool isAxisAligned(const SkScalerContext::Rec& rec) {
    return 0 == rec.fPreSkewX &&
        (bothZero(rec.fPost2x2[0][1], rec.fPost2x2[1][0]) ||
        bothZero(rec.fPost2x2[0][0], rec.fPost2x2[1][1]));
}

static bool needToRenderWithSkia(const SkScalerContext::Rec& rec) {
#ifdef SK_ENFORCE_ROTATED_TEXT_AA_ON_WINDOWS
    // What we really want to catch is when GDI will ignore the AA request and give
    // us BW instead. Smallish rotated text is one heuristic, so this code is just
    // an approximation. We shouldn't need to do this for larger sizes, but at those
    // sizes, the quality difference gets less and less between our general
    // scanconverter and GDI's.
    if (SkMask::kA8_Format == rec.fMaskFormat && !isAxisAligned(rec)) {
        return true;
    }
#endif
    // false means allow GDI to generate the bits
    return false;
}

static const LOGFONT& get_default_font() {
    static LOGFONT gDefaultFont;
    // don't hardcode on Windows, Win2000, XP, Vista, and international all have different default
    // and the user could change too


//  lfMessageFont is garbage on my XP, so skip for now
#if 0
    if (gDefaultFont.lfFaceName[0] != 0) {
        return gDefaultFont;
    }

    NONCLIENTMETRICS ncm;
    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);

    //memcpy(&gDefaultFont, &(ncm.lfMessageFont), sizeof(LOGFONT));
#endif

    return gDefaultFont;
}

static bool FindByLogFont(SkTypeface* face, SkTypeface::Style requestedStyle, void* ctx) {
    LogFontTypeface* lface = reinterpret_cast<LogFontTypeface*>(face);
    const LOGFONT* lf = reinterpret_cast<const LOGFONT*>(ctx);

    return getStyle(lface->fLogFont) == requestedStyle &&
           !memcmp(&lface->fLogFont, lf, sizeof(LOGFONT));
}

/**
 *  This guy is public. It first searches the cache, and if a match is not found,
 *  it creates a new face.
 */
SkTypeface* SkCreateTypefaceFromLOGFONT(const LOGFONT& origLF) {
    LOGFONT lf = origLF;
    make_canonical(&lf);
    SkTypeface* face = SkTypefaceCache::FindByProc(FindByLogFont, &lf);
    if (face) {
        face->ref();
    } else {
        face = LogFontTypeface::Create(lf);
        SkTypefaceCache::Add(face, getStyle(lf));
    }
    return face;
}

SkFontID SkFontHost::NextLogicalFont(SkFontID currFontID, SkFontID origFontID) {
  // Zero means that we don't have any fallback fonts for this fontID.
  // This function is implemented on Android, but doesn't have much
  // meaning here.
  return 0;
}

static void GetLogFontByID(SkFontID fontID, LOGFONT* lf) {
    LogFontTypeface* face = (LogFontTypeface*)SkTypefaceCache::FindByID(fontID);
    if (face) {
        *lf = face->fLogFont;
    } else {
        sk_bzero(lf, sizeof(LOGFONT));
    }
}

// Construct Glyph to Unicode table.
// Unicode code points that require conjugate pairs in utf16 are not
// supported.
// TODO(arthurhsu): Add support for conjugate pairs. It looks like that may
// require parsing the TTF cmap table (platform 4, encoding 12) directly instead
// of calling GetFontUnicodeRange().
static void populate_glyph_to_unicode(HDC fontHdc, const unsigned glyphCount,
                                      SkTDArray<SkUnichar>* glyphToUnicode) {
    DWORD glyphSetBufferSize = GetFontUnicodeRanges(fontHdc, NULL);
    if (!glyphSetBufferSize) {
        return;
    }

    SkAutoTDeleteArray<BYTE> glyphSetBuffer(new BYTE[glyphSetBufferSize]);
    GLYPHSET* glyphSet =
        reinterpret_cast<LPGLYPHSET>(glyphSetBuffer.get());
    if (GetFontUnicodeRanges(fontHdc, glyphSet) != glyphSetBufferSize) {
        return;
    }

    glyphToUnicode->setCount(glyphCount);
    memset(glyphToUnicode->begin(), 0, glyphCount * sizeof(SkUnichar));
    for (DWORD i = 0; i < glyphSet->cRanges; ++i) {
        // There is no guarantee that within a Unicode range, the corresponding
        // glyph id in a font file are continuous. So, even if we have ranges,
        // we can't just use the first and last entry of the range to compute
        // result. We need to enumerate them one by one.
        int count = glyphSet->ranges[i].cGlyphs;
        SkAutoTArray<WCHAR> chars(count + 1);
        chars[count] = 0;  // termintate string
        SkAutoTArray<WORD> glyph(count);
        for (USHORT j = 0; j < count; ++j) {
            chars[j] = glyphSet->ranges[i].wcLow + j;
        }
        GetGlyphIndicesW(fontHdc, chars.get(), count, glyph.get(),
                         GGI_MARK_NONEXISTING_GLYPHS);
        // If the glyph ID is valid, and the glyph is not mapped, then we will
        // fill in the char id into the vector. If the glyph is mapped already,
        // skip it.
        // TODO(arthurhsu): better improve this. e.g. Get all used char ids from
        // font cache, then generate this mapping table from there. It's
        // unlikely to have collisions since glyph reuse happens mostly for
        // different Unicode pages.
        for (USHORT j = 0; j < count; ++j) {
            if (glyph[j] != 0xffff && glyph[j] < glyphCount &&
                (*glyphToUnicode)[glyph[j]] == 0) {
                (*glyphToUnicode)[glyph[j]] = chars[j];
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////

class HDCOffscreen {
public:
    HDCOffscreen() {
        fFont = 0;
        fDC = 0;
        fBM = 0;
        fBits = NULL;
        fWidth = fHeight = 0;
        fIsBW = false;
    }

    ~HDCOffscreen() {
        if (fDC) {
            DeleteDC(fDC);
        }
        if (fBM) {
            DeleteObject(fBM);
        }
    }

    void init(HFONT font, const XFORM& xform) {
        fFont = font;
        fXform = xform;
    }

    const void* draw(const SkGlyph&, bool isBW, size_t* srcRBPtr);

private:
    HDC     fDC;
    HBITMAP fBM;
    HFONT   fFont;
    XFORM   fXform;
    void*   fBits;  // points into fBM
    int     fWidth;
    int     fHeight;
    bool    fIsBW;

    enum {
        // will always trigger us to reset the color, since we
        // should only store 0 or 0x00FFFFFF or gray (0x007F7F7F)
        kInvalid_Color = 12345
    };
};

static void sharpenImage (unsigned char* p_data, int DibWidth, int height)
{
    int h[3][3];////����(3x3)����
    h[0][0] = 1;  h[0][1] = 1; h[0][2] = 1;
    h[1][0] = 1;  h[1][1] = 1; h[1][2] = 1;
    h[2][0] = 1;  h[2][1] = 1; h[2][2] = 1;
    BYTE *p_temp=new BYTE[height*DibWidth];	// ��ʱ�����ڴ棬�Ա�����ͼ��
    for(int j=0;j<height-2;j++)	// ÿ��
    {
        for(int i=0;i<DibWidth-8;i++)	// ÿ��
        {
            double pby_pt=0;
            //��Ӧ�ĵ�0�е�ֵ���Ծ����Ӧֵ�������
            pby_pt= h[0][0]*(*(p_data+(height-j-1)*DibWidth+i))
                +h[0][1]*(*(p_data+(height-j-1)*DibWidth+i+3))
                +h[0][2]*(*(p_data+(height-j-1)*DibWidth+i+6))
                //��Ӧ�ĵ�1�е�ֵ���Ծ����Ӧֵ�������
                +h[1][0]*(*(p_data+(height-j-2)*DibWidth+i))
                +h[1][1]*(*(p_data+(height-j-2)*DibWidth+i+3))
                +h[1][2]*(*(p_data+(height-j-2)*DibWidth+i+6))
                //��Ӧ�ĵ�2�е�ֵ���Ծ����Ӧֵ�������
                +h[2][0]*(*(p_data+(height-j-3)*DibWidth+i))
                +h[2][1]*(*(p_data+(height-j-3)*DibWidth+i+3))
                +h[2][2]*(*(p_data+(height-j-3)*DibWidth+i+6));
            *(p_temp+(height-j-2)*DibWidth+i+3)=abs(int(pby_pt/9));//ȡ�ܺ͵ĵ�ƽ��ֵ
        }
    }
    memcpy(p_data,p_temp,height*DibWidth);  // ���ƴ������ͼ��
    delete []p_temp;//ɾ����ʱ�����ڴ�
}

const void* HDCOffscreen::draw(const SkGlyph& glyph, bool isBW, size_t* srcRBPtr) 
{
    if (0 == fDC) {
        fDC = CreateCompatibleDC(0);
        if (0 == fDC) {
            return NULL;
        }
        SetGraphicsMode(fDC, GM_ADVANCED);
        SetBkMode(fDC, TRANSPARENT);
        SetTextAlign(fDC, TA_LEFT | TA_BASELINE);
        SelectObject(fDC, fFont);

        COLORREF color = 0x00FFFFFF;
        COLORREF prev = SetTextColor(fDC, color);
        SkASSERT(prev != CLR_INVALID);
    }

    if (fBM && (fIsBW != isBW || fWidth < glyph.fWidth || fHeight < glyph.fHeight)) {
        DeleteObject(fBM);
        fBM = 0;
    }
    fIsBW = isBW;

    fWidth = SkMax32(fWidth, glyph.fWidth);
    fHeight = SkMax32(fHeight, glyph.fHeight);
    
    int biWidth = isBW ? alignTo32(fWidth) : fWidth;

    MyBitmapInfo info;
    if (0 == fBM) {
        sk_bzero(&info, sizeof(info));
        if (isBW) {
            RGBQUAD blackQuad = { 0, 0, 0, 0 };
            RGBQUAD whiteQuad = { 0xFF, 0xFF, 0xFF, 0 };
            info.bmiColors[0] = blackQuad;
            info.bmiColors[1] = whiteQuad;
        }
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = biWidth;
        info.bmiHeader.biHeight = fHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = isBW ? 1 : 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (isBW) {
            info.bmiHeader.biClrUsed = 2;
        }
        fBM = CreateDIBSection(fDC, &info, DIB_RGB_COLORS, &fBits, 0, 0);
        if (0 == fBM) {
            return NULL;
        }
        SelectObject(fDC, fBM);
    }

    // erase
    size_t srcRB = isBW ? (biWidth >> 3) : (fWidth << 2);
    size_t size = fHeight * srcRB;
    memset(fBits, 0, size);

    XFORM xform = fXform;
    xform.eDx = (float)-glyph.fLeft;
    xform.eDy = (float)-glyph.fTop;
    SetWorldTransform(fDC, &xform);

    uint16_t glyphID = glyph.getGlyphID();
    BOOL ret = ExtTextOutW(fDC, 0, 0, ETO_GLYPH_INDEX, NULL, reinterpret_cast<LPCWSTR>(&glyphID), 1, NULL);
    GdiFlush();
    if (0 == ret) {
        return NULL;}
    
    *srcRBPtr = srcRB;
    // offset to the start of the image
    return (const char*)fBits + (fHeight - glyph.fHeight) * srcRB;

}

//////////////////////////////////////////////////////////////////////////////////////////////

class SkScalerContext_Windows : public SkScalerContext {
public:
    SkScalerContext_Windows(const SkDescriptor* desc);
    virtual ~SkScalerContext_Windows();

protected:
    virtual unsigned generateGlyphCount();
    virtual uint16_t generateCharToGlyph(SkUnichar uni);
    virtual void generateAdvance(SkGlyph* glyph);
    virtual void generateMetrics(SkGlyph* glyph);
    virtual void generateImage(const SkGlyph& glyph);
    virtual void generatePath(const SkGlyph& glyph, SkPath* path);
    virtual void generateFontMetrics(SkPaint::FontMetrics* mX, SkPaint::FontMetrics* mY);
    //virtual SkDeviceContext getDC() {return ddc;}
private:
    HDCOffscreen fOffscreen;
	SkScalar	 fScale;	// to get from canonical size to real size
    MAT2         fMat22;
    XFORM        fXform;
    HDC          fDDC;
    HFONT        fSavefont;
    HFONT        fFont;
    SCRIPT_CACHE fSC;
    int          fGlyphCount;

    /**
     *  Some fonts need extra pixels added to avoid clipping, as the bounds
     *  returned by getOutlineMetrics does not match what GDI draws. Since
     *  this costs more RAM and therefore slower blits, we have a table to
     *  only do this for known "bad" fonts.
     */
    SkIRect      fOutset;

    HFONT        fHiResFont;
    MAT2         fMat22Identity;
    SkMatrix     fHiResMatrix;

    enum Type {
        kTrueType_Type, kBitmap_Type,
    } fType;
    TEXTMETRIC fTM;
};

static float mul2float(SkScalar a, SkScalar b) {
    return SkScalarToFloat(SkScalarMul(a, b));
}

static FIXED float2FIXED(float x) {
    return SkFixedToFIXED(SkFloatToFixed(x));
}

static SkMutex gFTMutex;

#define HIRES_TEXTSIZE  2048
#define HIRES_SHIFT     11
static inline SkFixed HiResToFixed(int value) {
    return value << (16 - HIRES_SHIFT);
}

static bool needHiResMetrics(const SkScalar mat[2][2]) {
    return mat[1][0] || mat[0][1];
}

static BYTE compute_quality(const SkScalerContext::Rec& rec) {
    switch (rec.fMaskFormat) {
        case SkMask::kBW_Format:
            return NONANTIALIASED_QUALITY;
        case SkMask::kLCD16_Format:
        case SkMask::kLCD32_Format:
            return CLEARTYPE_QUALITY;
        default:
            if (rec.fFlags & SkScalerContext::kGenA8FromLCD_Flag) {
                return CLEARTYPE_QUALITY;
            } else {
                return ANTIALIASED_QUALITY;
            }
    }
}

static void ensure_typeface_accessible(SkFontID fontID) {
    //     LogFontTypeface* face = static_cast<LogFontTypeface*>(SkTypefaceCache::FindByID(fontID));
    //     if (face) {
    //         SkFontHost::EnsureTypefaceAccessible(*face);
    //     }
    //__asm int 3;
}

SkScalerContext_Windows::SkScalerContext_Windows(const SkDescriptor* desc)
        : SkScalerContext(desc), fDDC(0), fFont(0), fSavefont(0), fSC(0)
        , fGlyphCount(-1) {
    SkAutoMutexAcquire  ac(gFTMutex);

    fDDC = ::CreateCompatibleDC(NULL);
    SetGraphicsMode(fDDC, GM_ADVANCED);
    SetBkMode(fDDC, TRANSPARENT);

    // Scaling by the DPI is inconsistent with how Skia draws elsewhere
    //SkScalar height = -(fRec.fTextSize * GetDeviceCaps(ddc, LOGPIXELSY) / 72);
    LOGFONT lf;
    GetLogFontByID(fRec.fFontID, &lf);
    lf.lfHeight = -gCanonicalTextSize;
    lf.lfQuality = DEFAULT_QUALITY; // compute_quality(fRec);
    fFont = CreateFontIndirect(&lf);

    if (!compute_bounds_outset(lf, &fOutset)) {
        fOutset.setEmpty();
    }

    // if we're rotated, or want fractional widths, create a hires font
    fHiResFont = 0;
    if (needHiResMetrics(fRec.fPost2x2)) {
        lf.lfHeight = -HIRES_TEXTSIZE;
        fHiResFont = CreateFontIndirect(&lf);

        fMat22Identity.eM11 = fMat22Identity.eM22 = SkFixedToFIXED(SK_Fixed1);
        fMat22Identity.eM12 = fMat22Identity.eM21 = SkFixedToFIXED(0);

        // construct a matrix to go from HIRES logical units to our device units
        fRec.getSingleMatrix(&fHiResMatrix);
        SkScalar scale = SkScalarInvert(SkIntToScalar(HIRES_TEXTSIZE));
        fHiResMatrix.preScale(scale, scale);
    }
    fSavefont = (HFONT)SelectObject(fDDC, fFont);

    if (0 == GetTextMetrics(fDDC, &fTM)) {
        ensure_typeface_accessible(fRec.fFontID);
        if (0 == GetTextMetrics(fDDC, &fTM)) {
            fTM.tmPitchAndFamily = TMPF_TRUETYPE;
        }
    }
    // Used a logfont on a memory context, should never get a device font.
    // Therefore all TMPF_DEVICE will be PostScript fonts.

    // If TMPF_VECTOR is set, one of TMPF_TRUETYPE or TMPF_DEVICE must be set,
    // otherwise we have a vector FON, which we don't support.
    // This was determined by testing with Type1 PFM/PFB and OpenTypeCFF OTF,
    // as well as looking at Wine bugs and sources.
    SkASSERT(!(fTM.tmPitchAndFamily & TMPF_VECTOR) ||
        (fTM.tmPitchAndFamily & (TMPF_TRUETYPE | TMPF_DEVICE)));

    if (fTM.tmPitchAndFamily & TMPF_VECTOR) {
        // Truetype or PostScript.
        // Stroked FON also gets here (TMPF_VECTOR), but we don't handle it.
        fType = SkScalerContext_Windows::kTrueType_Type;
        fScale = fRec.fTextSize / gCanonicalTextSize;

        fXform.eM11 = mul2float(fScale, fRec.fPost2x2[0][0]);
        fXform.eM12 = mul2float(fScale, fRec.fPost2x2[1][0]);
        fXform.eM21 = mul2float(fScale, fRec.fPost2x2[0][1]);
        fXform.eM22 = mul2float(fScale, fRec.fPost2x2[1][1]);
        fXform.eDx = 0;
        fXform.eDy = 0;

        fMat22.eM11 = float2FIXED(fXform.eM11);
        fMat22.eM12 = float2FIXED(fXform.eM12);
        fMat22.eM21 = float2FIXED(-fXform.eM21);
        fMat22.eM22 = float2FIXED(-fXform.eM22);

        if (needToRenderWithSkia(fRec)) {
            __asm int 3;
            //this->forceGenerateImageFromPath();
        }

    } else {
        // Assume bitmap
        fType = SkScalerContext_Windows::kBitmap_Type;
        fScale = SK_Scalar1;

        fXform.eM11 = 1.0f;
        fXform.eM12 = 0.0f;
        fXform.eM21 = 0.0f;
        fXform.eM22 = 1.0f;
        fXform.eDx = 0.0f;
        fXform.eDy = 0.0f;

        fMat22.eM11 = SkScalarToFIXED(fRec.fPost2x2[0][0]);
        fMat22.eM12 = SkScalarToFIXED(fRec.fPost2x2[1][0]);
        fMat22.eM21 = SkScalarToFIXED(-fRec.fPost2x2[0][1]);
        fMat22.eM22 = SkScalarToFIXED(-fRec.fPost2x2[1][1]);
//#define SkScalarCeilToInt(x) (((x) + (1 << 16) - 1) >> 16)
        

        lf.lfHeight = -(SkScalarCeilToInt(fRec.fTextSize));
        HFONT bitmapFont = CreateFontIndirect(&lf);
        SelectObject(fDDC, bitmapFont);
        ::DeleteObject(fFont);
        fFont = bitmapFont;

        if (0 == GetTextMetrics(fDDC, &fTM)) {
            ensure_typeface_accessible(fRec.fFontID);
            //if the following fails, we'll just draw at gCanonicalTextSize.
            GetTextMetrics(fDDC, &fTM);
        }
    }

    fOffscreen.init(fFont, fXform);
}

SkScalerContext_Windows::~SkScalerContext_Windows() {
    if (fDDC) {
        ::SelectObject(fDDC, fSavefont);
        ::DeleteDC(fDDC);
    }
    if (fFont) {
        ::DeleteObject(fFont);
    }
    if (fHiResFont) {
        ::DeleteObject(fHiResFont);
    }
    if (fSC) {
        ::ScriptFreeCache(&fSC);
    }
}

unsigned SkScalerContext_Windows::generateGlyphCount() {
    if (fGlyphCount < 0) {
        if (fType == SkScalerContext_Windows::kBitmap_Type) {
            return fTM.tmLastChar;
        }
        fGlyphCount = calculateOutlineGlyphCount(fDDC);
    }
    return fGlyphCount;
}

uint16_t SkScalerContext_Windows::generateCharToGlyph(SkUnichar uni) {
    uint16_t index = 0;
    WCHAR c[2];
    // TODO(ctguil): Support characters that generate more than one glyph.
    if (SkUTF16_FromUnichar(uni, (uint16_t*)c) == 1) {
        // Type1 fonts fail with uniscribe API. Use GetGlyphIndices for plane 0.
        SkAssertResult(GetGlyphIndicesW(fDDC, c, 1, &index, 0));
    } else {
        // Use uniscribe to detemine glyph index for non-BMP characters.
        // Need to add extra item to SCRIPT_ITEM to work around a bug in older
        // windows versions. https://bugzilla.mozilla.org/show_bug.cgi?id=366643
        SCRIPT_ITEM si[2 + 1];
        int items;
        SkAssertResult(
            SUCCEEDED(ScriptItemize(c, 2, 2, NULL, NULL, si, &items)));

        WORD log[2];
        SCRIPT_VISATTR vsa;
        int glyphs;
        SkAssertResult(SUCCEEDED(ScriptShape(
            fDDC, &fSC, c, 2, 1, &si[0].a, &index, log, &vsa, &glyphs)));
    }
    return index;
}

void SkScalerContext_Windows::generateAdvance(SkGlyph* glyph) {
    this->generateMetrics(glyph);
}

void SkScalerContext_Windows::generateMetrics(SkGlyph* glyph) {

    SkASSERT(fDDC);

    if (fType == SkScalerContext_Windows::kBitmap_Type) {
        SIZE size;
        WORD glyphs = glyph->getGlyphID(0);
        if (0 == GetTextExtentPointI(fDDC, &glyphs, 1, &size)) {
            glyph->fWidth = SkToS16(fTM.tmMaxCharWidth);
        } else {
            glyph->fWidth = SkToS16(size.cx);
        }
        glyph->fHeight = SkToS16(size.cy);

        glyph->fTop = SkToS16(-fTM.tmAscent);
        glyph->fLeft = SkToS16(0);
        glyph->fAdvanceX = SkIntToFixed(glyph->fWidth);
        glyph->fAdvanceY = 0;

        //Apply matrix to values.
        glyph->fAdvanceY = SkFixedMul(SkFIXEDToFixed(fMat22.eM21), glyph->fAdvanceX);
        glyph->fAdvanceX = SkFixedMul(SkFIXEDToFixed(fMat22.eM11), glyph->fAdvanceX);

        apply_outset(glyph, fOutset);
        return;
    }

    GLYPHMETRICS gm;
    sk_bzero(&gm, sizeof(gm));

    glyph->fRsbDelta = 0;
    glyph->fLsbDelta = 0;

    // Note: need to use GGO_GRAY8_BITMAP instead of GGO_METRICS because GGO_METRICS returns a smaller
    // BlackBlox; we need the bigger one in case we need the image.  fAdvance is the same.
    uint32_t ret = GetGlyphOutlineW(fDDC, glyph->getGlyphID(0), GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &gm, 0, NULL, &fMat22);
    if (GDI_ERROR == ret) {
        ensure_typeface_accessible(fRec.fFontID);
        ret = GetGlyphOutlineW(fDDC, glyph->getGlyphID(0), GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &gm, 0, NULL, &fMat22);
    }

    if (GDI_ERROR != ret) {
        if (ret == 0) {
            // for white space, ret is zero and gmBlackBoxX, gmBlackBoxY are 1 incorrectly!
            gm.gmBlackBoxX = gm.gmBlackBoxY = 0;
        }
        glyph->fWidth   = gm.gmBlackBoxX;
        glyph->fHeight  = gm.gmBlackBoxY;
        glyph->fTop     = SkToS16(gm.gmptGlyphOrigin.y - gm.gmBlackBoxY);
        glyph->fLeft    = SkToS16(gm.gmptGlyphOrigin.x);
        glyph->fAdvanceX = SkIntToFixed(gm.gmCellIncX);
        glyph->fAdvanceY = -SkIntToFixed(gm.gmCellIncY);

        // we outset in all dimensions, since the image may bleed outside
        // of the computed bounds returned by GetGlyphOutline.
        // This was deduced by trial and error for small text (e.g. 8pt), so there
        // maybe a more precise way to make this adjustment...
        //
        // This test shows us clipping the tops of some of the CJK fonts unless we
        // increase the top of the box by 2, hence the height by 4. This seems to
        // correspond to an embedded bitmap font, but not sure.
        //     LayoutTests/fast/text/backslash-to-yen-sign-euc.html
        //
        if (glyph->fWidth) {    // don't outset an empty glyph
            glyph->fWidth += 4;
            glyph->fHeight += 4;
            glyph->fTop -= 2;
            glyph->fLeft -= 2;

            apply_outset(glyph, fOutset);
        }

        if (fHiResFont) {
            SelectObject(fDDC, fHiResFont);
            sk_bzero(&gm, sizeof(gm));
            ret = GetGlyphOutlineW(fDDC, glyph->getGlyphID(0), GGO_METRICS | GGO_GLYPH_INDEX, &gm, 0, NULL, &fMat22Identity);
            if (GDI_ERROR != ret) {
                SkPoint advance;
                fHiResMatrix.mapXY(SkIntToScalar(gm.gmCellIncX), SkIntToScalar(gm.gmCellIncY), &advance);
                glyph->fAdvanceX = SkScalarToFixed(advance.fX);
                glyph->fAdvanceY = SkScalarToFixed(advance.fY);
            }
            SelectObject(fDDC, fFont);
        }
    } else {
        glyph->zeroMetrics();
    }
}

void SkScalerContext_Windows::generateFontMetrics(SkPaint::FontMetrics* mx, SkPaint::FontMetrics* my) {
// Note: This code was borrowed from generateLineHeight, which has a note
// stating that it may be incorrect.
    if (!(mx || my))
      return;

    SkASSERT(fDDC);

    OUTLINETEXTMETRIC otm;

    uint32_t ret = GetOutlineTextMetrics(fDDC, sizeof(otm), &otm);
    if (sizeof(otm) != ret) {
      return;
    }

    if (mx) {
        mx->fTop = -fScale * otm.otmTextMetrics.tmAscent;
		mx->fAscent = -fScale * otm.otmAscent;
		mx->fDescent = -fScale * otm.otmDescent;
		mx->fBottom = fScale * otm.otmTextMetrics.tmDescent;
		mx->fLeading = fScale * (otm.otmTextMetrics.tmInternalLeading
								 + otm.otmTextMetrics.tmExternalLeading);
    }

    if (my) {
		my->fTop = -fScale * otm.otmTextMetrics.tmAscent;
		my->fAscent = -fScale * otm.otmAscent;
		my->fDescent = -fScale * otm.otmDescent;
		my->fBottom = fScale * otm.otmTextMetrics.tmDescent;
		my->fLeading = fScale * (otm.otmTextMetrics.tmInternalLeading
								 + otm.otmTextMetrics.tmExternalLeading);
    }
}

////////////////////////////////////////////////////////////////////////////////////////

static void build_power_table(uint8_t table[], float ee) {
    for (int i = 0; i < 256; i++) {
        float x = i / 255.f;
        x = sk_float_pow(x, ee);
        int xx = SkScalarRound(SkFloatToScalar(x * 255));
        table[i] = SkToU8(xx);
    }
}

/**
 *  This will invert the gamma applied by GDI (gray-scale antialiased), so we
 *  can get linear values.
 *
 *  GDI grayscale appears to use a hard-coded gamma of 2.3.
 *
 *  GDI grayscale appears to draw using the black and white rasterizer at four
 *  times the size and then downsamples to compute the coverage mask. As a
 *  result there are only seventeen total grays. This lack of fidelity means
 *  that shifting into other color spaces is imprecise.
 */
static const uint8_t* getInverseGammaTableGDI() {
    static bool gInited = false;
    static uint8_t gTableGdi[256];
    if (!gInited) {
        build_power_table(gTableGdi, 2.3f);
        gInited = true;
    }
    return gTableGdi;
}

/**
 *  This will invert the gamma applied by GDI ClearType, so we can get linear
 *  values.
 *
 *  GDI ClearType uses SPI_GETFONTSMOOTHINGCONTRAST / 1000 as the gamma value.
 *  If this value is not specified, the default is a gamma of 1.4.
 */
static const uint8_t* getInverseGammaTableClearType() {
    static bool gInited;
    static uint8_t gTableClearType[256];
    if (!gInited) {
        UINT level = 0;
        if (!SystemParametersInfo(SPI_GETFONTSMOOTHINGCONTRAST, 0, &level, 0) || !level) {
            // can't get the data, so use a default
            level = 1400;
        }
        build_power_table(gTableClearType, level / 1000.0f);
        gInited = true;
    }
    return gTableClearType;
}

static const uint8_t* getInverseGammaTableMyClearType() {
    static bool gInited;
    static uint8_t gTableMyClearType[256];
    if (!gInited) {
        UINT level = 1000;
        build_power_table(gTableMyClearType, level / 1000.0f);
        gInited = true;
    }
    return gTableMyClearType;
}

#include "SkColorPriv.h"

static inline uint16_t rgb_to_lcd16(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >>  8) & 0xFF;
    int b = (rgb >>  0) & 0xFF;

    // invert, since we draw black-on-white, but we want the original
    // src mask values.
    r = 255 - r;
    g = 255 - g;
    b = 255 - b;
    return SkPackRGB16(SkR32ToR16(r), SkG32ToG16(g), SkB32ToB16(b));
}

//Cannot assume that the input rgb is gray due to possible setting of kGenA8FromLCD_Flag.
template<bool APPLY_PREBLEND>
static inline uint8_t rgb_to_a8(SkGdiRGB rgb, const uint8_t* table8) {
    U8CPU r = (rgb >> 16) & 0xFF;
    U8CPU g = (rgb >>  8) & 0xFF;
    U8CPU b = (rgb >>  0) & 0xFF;
    return sk_apply_lut_if<APPLY_PREBLEND>(SkComputeLuminance(r, g, b), table8);
}

template<bool APPLY_PREBLEND>
static inline uint16_t rgb_to_lcd16(SkGdiRGB rgb, const uint8_t* tableR,
                                    const uint8_t* tableG,
                                    const uint8_t* tableB) {
                                        U8CPU r = sk_apply_lut_if<APPLY_PREBLEND>((rgb >> 16) & 0xFF, tableR);
                                        U8CPU g = sk_apply_lut_if<APPLY_PREBLEND>((rgb >>  8) & 0xFF, tableG);
                                        U8CPU b = sk_apply_lut_if<APPLY_PREBLEND>((rgb >>  0) & 0xFF, tableB);
                                        return SkPack888ToRGB16(r, g, b);
}

template<bool APPLY_PREBLEND>
static inline SkPMColor rgb_to_lcd32(SkGdiRGB rgb, const uint8_t* tableR,
                                     const uint8_t* tableG,
                                     const uint8_t* tableB) {
                                         U8CPU r = sk_apply_lut_if<APPLY_PREBLEND>((rgb >> 16) & 0xFF, tableR);
                                         U8CPU g = sk_apply_lut_if<APPLY_PREBLEND>((rgb >>  8) & 0xFF, tableG);
                                         U8CPU b = sk_apply_lut_if<APPLY_PREBLEND>((rgb >>  0) & 0xFF, tableB);
                                         return SkPackARGB32(0xFF, r, g, b);
}

// Is this GDI color neither black nor white? If so, we have to keep this
// image as is, rather than smashing it down to a BW mask.
//
// returns int instead of bool, since we don't want/have to pay to convert
// the zero/non-zero value into a bool
static int is_not_black_or_white(SkGdiRGB c) {
    // same as (but faster than)
    //      c &= 0x00FFFFFF;
    //      return 0 == c || 0x00FFFFFF == c;
    return (c + (c & 1)) & 0x00FFFFFF;
}

static bool is_rgb_really_bw(const SkGdiRGB* src, int width, int height, int srcRB) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (is_not_black_or_white(src[x])) {
                return false;
            }
        }
        src = SkTAddByteOffset(src, srcRB);
    }
    return true;
}

// gdi's bitmap is upside-down, so we reverse dst walking in Y
// whenever we copy it into skia's buffer
static void rgb_to_bw(const SkGdiRGB* SK_RESTRICT src, size_t srcRB,
                      const SkGlyph& glyph) {
                          const int width = glyph.fWidth;
                          const size_t dstRB = (width + 7) >> 3;
                          uint8_t* SK_RESTRICT dst = (uint8_t*)((char*)glyph.fImage + (glyph.fHeight - 1) * dstRB);

                          int byteCount = width >> 3;
                          int bitCount = width & 7;

                          // adjust srcRB to skip the values in our byteCount loop,
                          // since we increment src locally there
                          srcRB -= byteCount * 8 * sizeof(SkGdiRGB);

                          for (int y = 0; y < glyph.fHeight; ++y) {
                              if (byteCount > 0) {
                                  for (int i = 0; i < byteCount; ++i) {
                                      unsigned byte = 0;
                                      byte |= src[0] & (1 << 7);
                                      byte |= src[1] & (1 << 6);
                                      byte |= src[2] & (1 << 5);
                                      byte |= src[3] & (1 << 4);
                                      byte |= src[4] & (1 << 3);
                                      byte |= src[5] & (1 << 2);
                                      byte |= src[6] & (1 << 1);
                                      byte |= src[7] & (1 << 0);
                                      dst[i] = byte;
                                      src += 8;
                                  }
                              }
                              if (bitCount > 0) {
                                  unsigned byte = 0;
                                  unsigned mask = 0x80;
                                  for (int i = 0; i < bitCount; i++) {
                                      byte |= src[i] & mask;
                                      mask >>= 1;
                                  }
                                  dst[byteCount] = byte;
                              }
                              src = SkTAddByteOffset(src, srcRB);
                              dst -= dstRB;
                          }
}

template<bool APPLY_PREBLEND>
static void rgb_to_a8(const SkGdiRGB* SK_RESTRICT src, size_t srcRB,
                      const SkGlyph& glyph, const uint8_t* table8) {
                          const size_t dstRB = glyph.rowBytes();
                          const int width = glyph.fWidth;
                          uint8_t* SK_RESTRICT dst = (uint8_t*)((char*)glyph.fImage + (glyph.fHeight - 1) * dstRB);

                          for (int y = 0; y < glyph.fHeight; y++) {
                              for (int i = 0; i < width; i++) {
                                  dst[i] = rgb_to_a8<APPLY_PREBLEND>(src[i], table8);
                              }
                              src = SkTAddByteOffset(src, srcRB);
                              dst -= dstRB;
                          }
}

template<bool APPLY_PREBLEND>
static void rgb_to_lcd16(const SkGdiRGB* SK_RESTRICT src, size_t srcRB, const SkGlyph& glyph,
                         const uint8_t* tableR, const uint8_t* tableG, const uint8_t* tableB) {
                             const size_t dstRB = glyph.rowBytes();
                             const int width = glyph.fWidth;
                             uint16_t* SK_RESTRICT dst = (uint16_t*)((char*)glyph.fImage + (glyph.fHeight - 1) * dstRB);

                             for (int y = 0; y < glyph.fHeight; y++) {
                                 for (int i = 0; i < width; i++) {
                                     dst[i] = rgb_to_lcd16<APPLY_PREBLEND>(src[i], tableR, tableG, tableB);
                                 }
                                 src = SkTAddByteOffset(src, srcRB);
                                 dst = (uint16_t*)((char*)dst - dstRB);
                             }
}

template<bool APPLY_PREBLEND>
static void rgb_to_lcd32(const SkGdiRGB* SK_RESTRICT src, size_t srcRB, const SkGlyph& glyph,
                         const uint8_t* tableR, const uint8_t* tableG, const uint8_t* tableB) {
                             const size_t dstRB = glyph.rowBytes();
                             const int width = glyph.fWidth;
                             uint32_t* SK_RESTRICT dst = (uint32_t*)((char*)glyph.fImage + (glyph.fHeight - 1) * dstRB);

                             for (int y = 0; y < glyph.fHeight; y++) {
                                 for (int i = 0; i < width; i++) {
                                     dst[i] = rgb_to_lcd32<APPLY_PREBLEND>(src[i], tableR, tableG, tableB);
                                 }
                                 src = SkTAddByteOffset(src, srcRB);
                                 dst = (uint32_t*)((char*)dst - dstRB);
                             }
}

static inline unsigned clamp255(unsigned x) {
    SkASSERT(x <= 256);
    return x - (x >> 8);
}


void SkScalerContext_Windows::generateImage(const SkGlyph& glyph) {
    SkAutoMutexAcquire ac(gFTMutex);
    SkASSERT(fDDC);

    const bool isBW = SkMask::kBW_Format == fRec.fMaskFormat;
    const bool isAA = !isLCD(fRec);

    size_t srcRB;
    const void* bits = fOffscreen.draw(glyph, isBW, &srcRB);
    if (NULL == bits) {
        ensure_typeface_accessible(fRec.fFontID);
        bits = fOffscreen.draw(glyph, isBW, &srcRB);
        if (NULL == bits) {
            sk_bzero(glyph.fImage, glyph.computeImageSize());
            return;
        }
    }
    
//     if (!isBW) {
//         const uint8_t* table;
//         //The offscreen contains a GDI blit if isAA and kGenA8FromLCD_Flag is not set.
//         //Otherwise the offscreen contains a ClearType blit.
// //         if (isAA && !(fRec.fFlags & SkScalerContext::kGenA8FromLCD_Flag)) {
// //             table = getInverseGammaTableGDI();
// //         } else {
// //             table = getInverseGammaTableClearType();
// //         }
//         table = getInverseGammaTableMyClearType(); // ǿ����cleartype
//         //Note that the following cannot really be integrated into the
//         //pre-blend, since we may not be applying the pre-blend; when we aren't
//         //applying the pre-blend it means that a filter wants linear anyway.
//         //Other code may also be applying the pre-blend, so we'd need another
//         //one with this and one without.
//         SkGdiRGB* addr = (SkGdiRGB*)bits;
//         for (int y = 0; y < glyph.fHeight; ++y) {
//             for (int x = 0; x < glyph.fWidth; ++x) {
//                 int r = (addr[x] >> 16) & 0xFF;
//                 int g = (addr[x] >>  8) & 0xFF;
//                 int b = (addr[x] >>  0) & 0xFF;
//                 addr[x] = (table[r] << 16) | (table[g] << 8) | table[b];
//             }
//             addr = SkTAddByteOffset(addr, srcRB);
//         }
//     }

    int width = glyph.fWidth;
    size_t dstRB = glyph.rowBytes();
    if (isBW) {
        const uint8_t* src = (const uint8_t*)bits;
        uint8_t* dst = (uint8_t*)((char*)glyph.fImage + (glyph.fHeight - 1) * dstRB);
        for (int y = 0; y < glyph.fHeight; y++) {
            memcpy(dst, src, dstRB);
            src += srcRB;
            dst -= dstRB;
        }
    } else if (isAA) {
        // since the caller may require A8 for maskfilters, we can't check for BW
        // ... until we have the caller tell us that explicitly
        const SkGdiRGB* src = (const SkGdiRGB*)bits;
        if (fPreBlend.isApplicable()) {
            rgb_to_a8<true>(src, srcRB, glyph, fPreBlend.fG);
        } else {
            rgb_to_a8<false>(src, srcRB, glyph, fPreBlend.fG);
        }
    } else {    // LCD16
        const SkGdiRGB* src = (const SkGdiRGB*)bits;
        if (is_rgb_really_bw(src, width, glyph.fHeight, srcRB)) {
            rgb_to_bw(src, srcRB, glyph);
            ((SkGlyph*)&glyph)->fMaskFormat = SkMask::kBW_Format;
        } else {
            if (SkMask::kLCD16_Format == glyph.fMaskFormat) {
                if (fPreBlend.isApplicable()) {
                    rgb_to_lcd16<true>(src, srcRB, glyph,
                        fPreBlend.fR, fPreBlend.fG, fPreBlend.fB);
                } else {
                    rgb_to_lcd16<false>(src, srcRB, glyph,
                        fPreBlend.fR, fPreBlend.fG, fPreBlend.fB);
                }
            } else {
                SkASSERT(SkMask::kLCD32_Format == glyph.fMaskFormat);
                if (fPreBlend.isApplicable()) {
                    rgb_to_lcd32<true>(src, srcRB, glyph,
                        fPreBlend.fR, fPreBlend.fG, fPreBlend.fB);
                } else {
                    rgb_to_lcd32<false>(src, srcRB, glyph,
                        fPreBlend.fR, fPreBlend.fG, fPreBlend.fB);
                }
            }
        }
    }
}


void SkScalerContext_Windows::generatePath(const SkGlyph& glyph, SkPath* path) {

    SkAutoMutexAcquire  ac(gFTMutex);

    SkASSERT(&glyph && path);
    SkASSERT(fDDC);

    path->reset();

#if 0
    char buf[1024];
    sprintf(buf, "generatePath: id:%d, w=%d, h=%d, font:%s,fh:%d\n", glyph.fID, glyph.fWidth, glyph.fHeight, lf.lfFaceName, lf.lfHeight);
    OutputDebugString(buf);
#endif

    GLYPHMETRICS gm;
    uint32_t total_size = GetGlyphOutlineW(fDDC, glyph.fID, GGO_NATIVE | GGO_GLYPH_INDEX, &gm, BUFFERSIZE, glyphbuf, &fMat22);

    if (GDI_ERROR != total_size) {

        const uint8_t* cur_glyph = glyphbuf;
        const uint8_t* end_glyph = glyphbuf + total_size;

        while(cur_glyph < end_glyph) {
            const TTPOLYGONHEADER* th = (TTPOLYGONHEADER*)cur_glyph;

            const uint8_t* end_poly = cur_glyph + th->cb;
            const uint8_t* cur_poly = cur_glyph + sizeof(TTPOLYGONHEADER);

            path->moveTo(SkFixedToScalar(*(SkFixed*)(&th->pfxStart.x)), SkFixedToScalar(*(SkFixed*)(&th->pfxStart.y)));

            while(cur_poly < end_poly) {
                const TTPOLYCURVE* pc = (const TTPOLYCURVE*)cur_poly;

                if (pc->wType == TT_PRIM_LINE) {
                    for (uint16_t i = 0; i < pc->cpfx; i++) {
                        path->lineTo(SkFixedToScalar(*(SkFixed*)(&pc->apfx[i].x)), SkFixedToScalar(*(SkFixed*)(&pc->apfx[i].y)));
                    }
                }

                if (pc->wType == TT_PRIM_QSPLINE) {
                    for (uint16_t u = 0; u < pc->cpfx - 1; u++) { // Walk through points in spline
                        POINTFX pnt_b = pc->apfx[u];    // B is always the current point
                        POINTFX pnt_c = pc->apfx[u+1];

                        if (u < pc->cpfx - 2) {          // If not on last spline, compute C
                            pnt_c.x = SkFixedToFIXED(SkFixedAve(*(SkFixed*)(&pnt_b.x), *(SkFixed*)(&pnt_c.x)));
                            pnt_c.y = SkFixedToFIXED(SkFixedAve(*(SkFixed*)(&pnt_b.y), *(SkFixed*)(&pnt_c.y)));
                        }

                        path->quadTo(SkFixedToScalar(*(SkFixed*)(&pnt_b.x)), SkFixedToScalar(*(SkFixed*)(&pnt_b.y)), SkFixedToScalar(*(SkFixed*)(&pnt_c.x)), SkFixedToScalar(*(SkFixed*)(&pnt_c.y)));
                    }
                }
                cur_poly += sizeof(uint16_t) * 2 + sizeof(POINTFX) * pc->cpfx;
            }
            cur_glyph += th->cb;
            path->close();
        }
    }
    else {
        SkASSERT(false);
    }
    //char buf[1024];
    //sprintf(buf, "generatePath: count:%d\n", count);
    //OutputDebugString(buf);
}

void SkFontHost::Serialize(const SkTypeface* face, SkWStream* stream) {
    SkASSERT(!"SkFontHost::Serialize unimplemented");
}

SkTypeface* SkFontHost::Deserialize(SkStream* stream) {
    SkASSERT(!"SkFontHost::Deserialize unimplemented");
    return NULL;
}

static bool getWidthAdvance(HDC hdc, int gId, int16_t* advance) {
    // Initialize the MAT2 structure to the identify transformation matrix.
    static const MAT2 mat2 = {SkScalarToFIXED(1), SkScalarToFIXED(0),
                        SkScalarToFIXED(0), SkScalarToFIXED(1)};
    int flags = GGO_METRICS | GGO_GLYPH_INDEX;
    GLYPHMETRICS gm;
    if (GDI_ERROR == GetGlyphOutline(hdc, gId, flags, &gm, 0, NULL, &mat2)) {
        return false;
    }
    SkASSERT(advance);
    *advance = gm.gmCellIncX;
    return true;
}

// static
SkAdvancedTypefaceMetrics* SkFontHost::GetAdvancedTypefaceMetrics(
        uint32_t fontID,
        SkAdvancedTypefaceMetrics::PerGlyphInfo perGlyphInfo) {
    LOGFONT lf;
    GetLogFontByID(fontID, &lf);
    SkAdvancedTypefaceMetrics* info = NULL;

    HDC hdc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontIndirect(&lf);
    HFONT savefont = (HFONT)SelectObject(hdc, font);
    HFONT designFont = NULL;

    // To request design units, create a logical font whose height is specified
    // as unitsPerEm.
    OUTLINETEXTMETRIC otm;
    if (!GetOutlineTextMetrics(hdc, sizeof(otm), &otm) ||
        !GetTextFace(hdc, LF_FACESIZE, lf.lfFaceName)) {
        goto Error;
    }
    lf.lfHeight = -SkToS32(otm.otmEMSquare);
    designFont = CreateFontIndirect(&lf);
    SelectObject(hdc, designFont);
    if (!GetOutlineTextMetrics(hdc, sizeof(otm), &otm)) {
        goto Error;
    }
    const unsigned glyphCount = calculateGlyphCount(hdc);

    //info = new SkAdvancedTypefaceMetrics;
    info = SkNEW(SkAdvancedTypefaceMetrics); // sk_weolar
    info->fEmSize = otm.otmEMSquare;
    info->fMultiMaster = false;
    info->fLastGlyphID = SkToU16(glyphCount - 1);
    info->fStyle = 0;
#ifdef UNICODE
    // Get the buffer size needed first.
    size_t str_len = WideCharToMultiByte(CP_UTF8, 0, lf.lfFaceName, -1, NULL,
                                         0, NULL, NULL);
    // Allocate a buffer (str_len already has terminating null accounted for).
    char *familyName = new char[str_len];
    // Now actually convert the string.
    WideCharToMultiByte(CP_UTF8, 0, lf.lfFaceName, -1, familyName, str_len,
                          NULL, NULL);
    info->fFontName.set(familyName);
    delete [] familyName;
#else
    info->fFontName.set(lf.lfFaceName);
#endif

    if (perGlyphInfo & SkAdvancedTypefaceMetrics::kToUnicode_PerGlyphInfo) {
        populate_glyph_to_unicode(hdc, glyphCount, &(info->fGlyphToUnicode));
    }

    if (otm.otmTextMetrics.tmPitchAndFamily & TMPF_TRUETYPE) {
        info->fType = SkAdvancedTypefaceMetrics::kTrueType_Font;
    } else {
        info->fType = SkAdvancedTypefaceMetrics::kOther_Font;
        info->fItalicAngle = 0;
        info->fAscent = 0;
        info->fDescent = 0;
        info->fStemV = 0;
        info->fCapHeight = 0;
        info->fBBox = SkIRect::MakeEmpty();
        return info;
    }
    
    // If this bit is clear the font is a fixed pitch font.
    if (!(otm.otmTextMetrics.tmPitchAndFamily & TMPF_FIXED_PITCH)) {
        info->fStyle |= SkAdvancedTypefaceMetrics::kFixedPitch_Style;
    }
    if (otm.otmTextMetrics.tmItalic) {
        info->fStyle |= SkAdvancedTypefaceMetrics::kItalic_Style;
    }
    // Setting symbolic style by default for now.
    info->fStyle |= SkAdvancedTypefaceMetrics::kSymbolic_Style;
    if (otm.otmTextMetrics.tmPitchAndFamily & FF_ROMAN) {
        info->fStyle |= SkAdvancedTypefaceMetrics::kSerif_Style;
    } else if (otm.otmTextMetrics.tmPitchAndFamily & FF_SCRIPT) {
            info->fStyle |= SkAdvancedTypefaceMetrics::kScript_Style;
    }

    // The main italic angle of the font, in tenths of a degree counterclockwise
    // from vertical.
    info->fItalicAngle = otm.otmItalicAngle / 10;
    info->fAscent = SkToS16(otm.otmTextMetrics.tmAscent);
    info->fDescent = SkToS16(-otm.otmTextMetrics.tmDescent);
    // TODO(ctguil): Use alternate cap height calculation.
    // MSDN says otmsCapEmHeight is not support but it is returning a value on
    // my Win7 box.
    info->fCapHeight = otm.otmsCapEmHeight;
    info->fBBox =
        SkIRect::MakeLTRB(otm.otmrcFontBox.left, otm.otmrcFontBox.top,
                          otm.otmrcFontBox.right, otm.otmrcFontBox.bottom);

    // Figure out a good guess for StemV - Min width of i, I, !, 1.
    // This probably isn't very good with an italic font.
    int16_t min_width = SHRT_MAX;
    info->fStemV = 0;
    char stem_chars[] = {'i', 'I', '!', '1'};
    for (size_t i = 0; i < SK_ARRAY_COUNT(stem_chars); i++) {
        ABC abcWidths;
        if (GetCharABCWidths(hdc, stem_chars[i], stem_chars[i], &abcWidths)) {
            int16_t width = abcWidths.abcB;
            if (width > 0 && width < min_width) {
                min_width = width;
                info->fStemV = min_width;
            }
        }
    }

    // If bit 1 is set, the font may not be embedded in a document.
    // If bit 1 is clear, the font can be embedded.
    // If bit 2 is set, the embedding is read-only.
    if (otm.otmfsType & 0x1) {
        info->fType = SkAdvancedTypefaceMetrics::kNotEmbeddable_Font;
    } else if (perGlyphInfo &
               SkAdvancedTypefaceMetrics::kHAdvance_PerGlyphInfo) {
        info->fGlyphWidths.reset(
            getAdvanceData(hdc, glyphCount, &getWidthAdvance));
    }

Error:
    SelectObject(hdc, savefont);
    DeleteObject(designFont);
    DeleteObject(font);
    DeleteDC(hdc);

    return info;
}

SkTypeface* SkFontHost::CreateTypefaceFromStream(SkStream* stream) {

    //Should not be used on Windows, keep linker happy
    SkASSERT(false);
    return SkCreateTypefaceFromLOGFONT(get_default_font());
}

SkStream* SkFontHost::OpenStream(SkFontID uniqueID) {
    const DWORD kTTCTag =
        SkEndian_SwapBE32(SkSetFourByteTag('t', 't', 'c', 'f'));
    LOGFONT lf;
    GetLogFontByID(uniqueID, &lf);

    HDC hdc = ::CreateCompatibleDC(NULL);
    HFONT font = CreateFontIndirect(&lf);
    HFONT savefont = (HFONT)SelectObject(hdc, font);

    SkMemoryStream* stream = NULL;
    DWORD tables[2] = {kTTCTag, 0};
    for (int i = 0; i < SK_ARRAY_COUNT(tables); i++) {
        size_t bufferSize = GetFontData(hdc, tables[i], 0, NULL, 0);
        if (bufferSize != GDI_ERROR) {
            //stream = new SkMemoryStream(bufferSize);  // sk_weolar
            stream = SkNEW_ARGS(SkMemoryStream, (bufferSize));
            if (GetFontData(hdc, tables[i], 0, (void*)stream->getMemoryBase(),
                            bufferSize)) {
                break;
            } else {
                //delete stream;
                SkDELETE(stream, SkMemoryStream);
                stream = NULL;
            }
        }
    }

    SelectObject(hdc, savefont);
    DeleteObject(font);
    DeleteDC(hdc);

    return stream;
}

SkScalerContext* SkFontHost::CreateScalerContext(const SkDescriptor* desc) {
    return SkNEW_ARGS(SkScalerContext_Windows, (desc));
}

/** Return the closest matching typeface given either an existing family
 (specified by a typeface in that family) or by a familyName, and a
 requested style.
 1) If familyFace is null, use famillyName.
 2) If famillyName is null, use familyFace.
 3) If both are null, return the default font that best matches style
 This MUST not return NULL.
 */

SkTypeface* SkFontHost::CreateTypeface(const SkTypeface* familyFace,
                                       const char familyName[],
                                       const void* data, size_t bytelength,
                                       SkTypeface::Style style) {
    LOGFONT lf;
    if (NULL == familyFace && NULL == familyName) {
        lf = get_default_font();
    } else if (familyFace) {
        LogFontTypeface* face = (LogFontTypeface*)familyFace;
        lf = face->fLogFont;
    } else {
        memset(&lf, 0, sizeof(LOGFONT));
#ifdef UNICODE
        // Get the buffer size needed first.
        size_t str_len = ::MultiByteToWideChar(CP_UTF8, 0, familyName,
                                                -1, NULL, 0);
        // Allocate a buffer (str_len already has terminating null
        // accounted for).
        wchar_t *wideFamilyName = new wchar_t[str_len];
        // Now actually convert the string.
        ::MultiByteToWideChar(CP_UTF8, 0, familyName, -1,
                                wideFamilyName, str_len);
        ::wcsncpy(lf.lfFaceName, wideFamilyName, LF_FACESIZE);
        delete [] wideFamilyName;
#else
        ::strncpy(lf.lfFaceName, familyName, LF_FACESIZE);
#endif
        lf.lfFaceName[LF_FACESIZE-1] = '\0';
    }
    setStyle(&lf, style);
    return SkCreateTypefaceFromLOGFONT(lf);
}

size_t SkFontHost::ShouldPurgeFontCache(size_t sizeAllocatedSoFar) {
    if (sizeAllocatedSoFar > FONT_CACHE_MEMORY_BUDGET)
        return sizeAllocatedSoFar - FONT_CACHE_MEMORY_BUDGET;
    else
        return 0;   // nothing to do
}

int SkFontHost::ComputeGammaFlag(const SkPaint& paint) {
    return 0;
}

void SkFontHost::GetGammaTables(const uint8_t* tables[2]) {
    tables[0] = NULL;   // black gamma (e.g. exp=1.4)
    tables[1] = NULL;   // white gamma (e.g. exp= 1/1.4)
}

SkTypeface* SkFontHost::CreateTypefaceFromFile(const char path[]) {
    printf("SkFontHost::CreateTypefaceFromFile unimplemented");
    return NULL;
}

void SkFontHost::FilterRec(SkScalerContext::Rec* rec) {
    // We don't control the hinting nor ClearType settings here
    rec->setHinting(SkPaint::kNormal_Hinting); // weolar

    // we do support LCD16
    if (SkMask::kLCD16_Format == rec->fMaskFormat) {
        return;
    }

    if (SkMask::FormatIsLCD((SkMask::Format)rec->fMaskFormat)) {
        rec->fMaskFormat = SkMask::kA8_Format;
    }
}

#endif // WIN32