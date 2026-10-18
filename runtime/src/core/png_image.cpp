#include "core/png_image.h"

#include <csetjmp>
#include <cstddef>
#include <cstring>

#include "png.h"

namespace tox2 {
namespace {

// libpng reports errors by longjmp, which runs no destructors. Each guarded function below holds
// only trivially destructible locals, and the caller owns every container, so a jump leaks nothing.
struct PngJob {
    png_structp png = nullptr;
    png_infop info = nullptr;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
};

void readData(png_structp png, png_bytep out, size_t count) {
    auto* job = static_cast<PngJob*>(png_get_io_ptr(png));
    if (count > job->size - job->pos) png_error(png, "unexpected end of data");
    std::memcpy(out, job->data + job->pos, count);
    job->pos += count;
}

void errorCallback(png_structp png, png_const_charp) { png_longjmp(png, 1); }

// Warnings (bad ancillary chunks and the like) do not fail a decode.
void warningCallback(png_structp, png_const_charp) {}

bool openJob(PngJob* job, std::span<const std::uint8_t> data) {
    job->data = data.data();
    job->size = data.size();
    job->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, job, errorCallback, warningCallback);
    if (job->png == nullptr) return false;
    job->info = png_create_info_struct(job->png);
    if (job->info == nullptr) {
        png_destroy_read_struct(&job->png, nullptr, nullptr);
        return false;
    }
    png_set_read_fn(job->png, job, readData);
    return true;
}

void closeJob(PngJob* job) { png_destroy_read_struct(&job->png, &job->info, nullptr); }

// Reads up to the first pixel row. Owns a setjmp.
bool readHeader(PngJob* job, std::uint32_t* width, std::uint32_t* height) {
    if (setjmp(png_jmpbuf(job->png)) != 0) return false;
    png_read_info(job->png, job->info);
    *width = png_get_image_width(job->png, job->info);
    *height = png_get_image_height(job->png, job->info);
    return true;
}

// Expands every colour type and depth to 8-bit RGBA and decodes into `rows`, which hold `height`
// rows of width * 4 bytes. Continues from readHeader on the same structures. Owns a setjmp.
bool readPixels(PngJob* job, png_bytep* rows, std::size_t row_bytes) {
    png_structp png = job->png;
    png_infop info = job->info;
    if (setjmp(png_jmpbuf(png)) != 0) return false;

    png_set_expand(png);  // palette -> RGB, gray < 8 bits -> 8, tRNS -> alpha channel
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);  // a no-op when alpha is already present
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != row_bytes) return false;
    png_read_image(png, rows);
    png_read_end(png, nullptr);
    return true;
}

}  // namespace

std::optional<RgbaImage> decodePngRgba8(std::span<const std::uint8_t> png, std::uint32_t max_side) {
    PngJob job;
    if (!openJob(&job, png)) return std::nullopt;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!readHeader(&job, &width, &height) || width == 0 || height == 0 || width > max_side || height > max_side) {
        closeJob(&job);
        return std::nullopt;
    }

    RgbaImage image;
    image.width = width;
    image.height = height;
    const std::size_t row_bytes = std::size_t{width} * 4;
    image.pixels.resize(row_bytes * height);
    std::vector<png_bytep> rows(height);
    for (std::uint32_t row = 0; row < height; row++) rows[row] = image.pixels.data() + row * row_bytes;

    const bool ok = readPixels(&job, rows.data(), row_bytes);
    closeJob(&job);
    if (!ok) return std::nullopt;
    return image;
}

}  // namespace tox2
