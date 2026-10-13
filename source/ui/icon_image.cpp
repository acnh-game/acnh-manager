#include "ui/icon_image.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jpeglib.h>

namespace acnh_manager::ui {
namespace {

constexpr int kMaxDimension = 1024;

/* libjpeg's default error handler prints and calls exit(); in an applet that is a black screen.
   One jump buffer per decode takes its place. */
struct ErrorHandler {
    jpeg_error_mgr base;
    std::jmp_buf jump;
};

void OnError(j_common_ptr info) {
    auto *handler = reinterpret_cast<ErrorHandler *>(info->err);
    std::longjmp(handler->jump, 1);
}

void OnMessage(j_common_ptr) { /* warnings stay silent: a cover is not worth a log line */ }

}  // namespace

bool DecodeJpeg(const std::uint8_t *data, std::size_t size, Bitmap *out) {
    out->width = 0;
    out->height = 0;
    out->rgba.clear();
    if (data == nullptr || size == 0) {
        return false;
    }

    jpeg_decompress_struct info{};
    ErrorHandler handler{};
    info.err = jpeg_std_error(&handler.base);
    handler.base.error_exit = OnError;
    handler.base.output_message = OnMessage;
    /* Everything allocated here is freed by hand: a longjmp out of a C++ frame would skip the
       destructors, so the buffers below are malloc/free rather than std::vector. */
    std::uint8_t *rgba = nullptr;
    std::uint8_t *row = nullptr;
    if (setjmp(handler.jump) != 0) {
        std::free(rgba);
        std::free(row);
        jpeg_destroy_decompress(&info);
        return false;
    }

    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&info);
        return false;
    }
    info.out_color_space = JCS_RGB; /* the cover JPEG is YCbCr; RGB is what the blit wants */
    jpeg_start_decompress(&info);
    const int width = static_cast<int>(info.output_width);
    const int height = static_cast<int>(info.output_height);
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension ||
        info.output_components != 3) {
        jpeg_destroy_decompress(&info);
        return false;
    }
    rgba = static_cast<std::uint8_t *>(std::malloc(static_cast<std::size_t>(width) * height * 4));
    row = static_cast<std::uint8_t *>(std::malloc(static_cast<std::size_t>(width) * 3));
    if (rgba == nullptr || row == nullptr) {
        std::free(rgba);
        std::free(row);
        jpeg_destroy_decompress(&info);
        return false;
    }
    while (info.output_scanline < info.output_height) {
        std::uint8_t *rows[1] = {row};
        jpeg_read_scanlines(&info, rows, 1);
        const int y = static_cast<int>(info.output_scanline) - 1;
        for (int x = 0; x < width; ++x) {
            std::uint8_t *target = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
            const std::uint8_t *source = row + static_cast<std::size_t>(x) * 3;
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
            target[3] = 0xFF;
        }
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    std::free(row);

    out->width = width;
    out->height = height;
    out->rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    std::free(rgba);
    return true;
}

}  // namespace acnh_manager::ui
