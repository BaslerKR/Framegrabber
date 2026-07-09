#include "FramegrabberGraphicsImageAdapter.h"

namespace FramegrabberGraphicsImageAdapter
{
GraphicsImagePixelFormat pixelFormatFromFgFormat(const int fgFormat) noexcept
{
    switch (fgFormat)
    {
    case FG_GRAY:
    case FG_GRAY_PLUS_PICNR:
        return GraphicsImagePixelFormat::Mono8;
    case FG_GRAY10:
        return GraphicsImagePixelFormat::Mono10Packed;
    case FG_GRAY12:
        return GraphicsImagePixelFormat::Mono12Packed;
    case FG_GRAY14:
        return GraphicsImagePixelFormat::Mono14Packed;
    case FG_GRAY16:
    case FG_GRAY16_PLUS_PICNR:
        return GraphicsImagePixelFormat::Mono16;
    case FG_GRAY32:
        return GraphicsImagePixelFormat::Mono32;
    case FG_BINARY:
        return GraphicsImagePixelFormat::Binary;
    case FG_COL24:
        return GraphicsImagePixelFormat::BGR24;
    case FG_COL32:
        return GraphicsImagePixelFormat::RGBA32;
    case FG_COL48:
        return GraphicsImagePixelFormat::BGR48;
    case FG_RGB8:
        return GraphicsImagePixelFormat::RGB24;
    case FG_RGB10:
        return GraphicsImagePixelFormat::RGB10Packed;
    case FG_RGB12:
        return GraphicsImagePixelFormat::RGB12Packed;
    case FG_RGB14:
        return GraphicsImagePixelFormat::RGB14Packed;
    case FG_RGB16:
        return GraphicsImagePixelFormat::RGB48;
    case FG_RGBA8:
        return GraphicsImagePixelFormat::RGBA32;
    case FG_RGBA10:
        return GraphicsImagePixelFormat::RGBA10Packed;
    case FG_RGBA12:
        return GraphicsImagePixelFormat::RGBA12Packed;
    case FG_RGBA14:
        return GraphicsImagePixelFormat::RGBA14Packed;
    case FG_RGBA16:
        return GraphicsImagePixelFormat::RGBA64;
    case FG_BGRA8:
        return GraphicsImagePixelFormat::BGRA32;
    case FG_BGRA10:
        return GraphicsImagePixelFormat::BGRA10Packed;
    case FG_BGRA12:
        return GraphicsImagePixelFormat::BGRA12Packed;
    case FG_BGRA14:
        return GraphicsImagePixelFormat::BGRA14Packed;
    case FG_BGRA16:
        return GraphicsImagePixelFormat::BGRA64;
    case FG_COL30:
        return GraphicsImagePixelFormat::BGR10Packed;
    case FG_COL36:
        return GraphicsImagePixelFormat::BGR12Packed;
    case FG_COL42:
        return GraphicsImagePixelFormat::BGR14Packed;
    case FG_RGBX32:
        return GraphicsImagePixelFormat::RGBX32;
    case FG_RGBX40:
        return GraphicsImagePixelFormat::RGBX10Packed;
    case FG_RGBX48:
        return GraphicsImagePixelFormat::RGBX12Packed;
    case FG_RGBX56:
        return GraphicsImagePixelFormat::RGBX14Packed;
    case FG_RGBX64:
        return GraphicsImagePixelFormat::RGBX64;
    case YUV422_8:
    case FG_YUV422_8:
    case FG_YCBCR422_8:
        return GraphicsImagePixelFormat::YCbCr422_8;
    case FG_BAYERGR8:
        return GraphicsImagePixelFormat::BayerGR8;
    case FG_BAYERGR10:
        return GraphicsImagePixelFormat::BayerGR10;
    case FG_BAYERGR12:
        return GraphicsImagePixelFormat::BayerGR12;
    case FG_BAYERGR14:
        return GraphicsImagePixelFormat::BayerGR14;
    case FG_BAYERGR16:
        return GraphicsImagePixelFormat::BayerGR16;
    case FG_BAYERRG8:
        return GraphicsImagePixelFormat::BayerRG8;
    case FG_BAYERRG10:
        return GraphicsImagePixelFormat::BayerRG10;
    case FG_BAYERRG12:
        return GraphicsImagePixelFormat::BayerRG12;
    case FG_BAYERRG14:
        return GraphicsImagePixelFormat::BayerRG14;
    case FG_BAYERRG16:
        return GraphicsImagePixelFormat::BayerRG16;
    case FG_BAYERGB8:
        return GraphicsImagePixelFormat::BayerGB8;
    case FG_BAYERGB10:
        return GraphicsImagePixelFormat::BayerGB10;
    case FG_BAYERGB12:
        return GraphicsImagePixelFormat::BayerGB12;
    case FG_BAYERGB14:
        return GraphicsImagePixelFormat::BayerGB14;
    case FG_BAYERGB16:
        return GraphicsImagePixelFormat::BayerGB16;
    case FG_BAYERBG8:
        return GraphicsImagePixelFormat::BayerBG8;
    case FG_BAYERBG10:
        return GraphicsImagePixelFormat::BayerBG10;
    case FG_BAYERBG12:
        return GraphicsImagePixelFormat::BayerBG12;
    case FG_BAYERBG14:
        return GraphicsImagePixelFormat::BayerBG14;
    case FG_BAYERBG16:
        return GraphicsImagePixelFormat::BayerBG16;
    case FG_BICOLOR_RGBG8:
        return GraphicsImagePixelFormat::BiColorRGBG8;
    case FG_BICOLOR_RGBG10:
        return GraphicsImagePixelFormat::BiColorRGBG10;
    case FG_BICOLOR_RGBG12:
        return GraphicsImagePixelFormat::BiColorRGBG12;
    case FG_BICOLOR_GRGB8:
        return GraphicsImagePixelFormat::BiColorGRGB8;
    case FG_BICOLOR_GRGB10:
        return GraphicsImagePixelFormat::BiColorGRGB10;
    case FG_BICOLOR_GRGB12:
        return GraphicsImagePixelFormat::BiColorGRGB12;
    case FG_BICOLOR_BGRG8:
        return GraphicsImagePixelFormat::BiColorBGRG8;
    case FG_BICOLOR_BGRG10:
        return GraphicsImagePixelFormat::BiColorBGRG10;
    case FG_BICOLOR_BGRG12:
        return GraphicsImagePixelFormat::BiColorBGRG12;
    case FG_BICOLOR_GBGR8:
        return GraphicsImagePixelFormat::BiColorGBGR8;
    case FG_BICOLOR_GBGR10:
        return GraphicsImagePixelFormat::BiColorGBGR10;
    case FG_BICOLOR_GBGR12:
        return GraphicsImagePixelFormat::BiColorGBGR12;
    case FG_RAW:
        return GraphicsImagePixelFormat::Raw;
    case FG_JPEG:
        return GraphicsImagePixelFormat::Jpeg;
    default:
        return GraphicsImagePixelFormat::Unknown;
    }
}

GraphicsImage wrapImage(
    const Framegrabber::Image& image,
    const std::size_t frameSeq) noexcept
{
    GraphicsImage frame;
    frame.storage = image.storage;
    frame.size = image.size;
    frame.width = image.width;
    frame.height = image.height;
    frame.stride = image.stride;
    frame.bitsPerPixel = image.bitsPerPixel;
    frame.sourceFormat = image.fgFormat;
    frame.pixelFormat = pixelFormatFromFgFormat(image.fgFormat);
    frame.frameSequence = frameSeq;
    return frame;
}
}
