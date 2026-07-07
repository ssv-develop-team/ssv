#include "ssv_preprocessor.hpp"

#include <algorithm>
#include <stdexcept>

namespace ssv::infer {

namespace {

int checked_dim(const TensorSpec &spec, size_t index, const char *name)
{
    if (spec.shape.size() <= index || spec.shape[index] <= 0)
        throw std::invalid_argument(std::string("model input shape has invalid ") + name);
    return static_cast<int>(spec.shape[index]);
}

void preprocess_bgr_to_chw(const uint8_t *src, int src_w, int src_h, int src_stride,
                           float *dst, int dst_w, int dst_h)
{
    float scale = std::min(static_cast<float>(dst_w) / src_w, static_cast<float>(dst_h) / src_h);
    int new_w = static_cast<int>(src_w * scale);
    int new_h = static_cast<int>(src_h * scale);
    int pad_x = (dst_w - new_w) / 2;
    int pad_y = (dst_h - new_h) / 2;

    size_t plane = static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);
    std::fill(dst, dst + 3 * plane, 114.0f / 255.0f);

    float *dst_r = dst;
    float *dst_g = dst + plane;
    float *dst_b = dst + 2 * plane;

    for (int y = 0; y < new_h; ++y) {
        int sy = static_cast<int>(y / scale);
        if (sy >= src_h)
            sy = src_h - 1;
        const uint8_t *src_row = src + sy * src_stride;
        int dy = y + pad_y;
        for (int x = 0; x < new_w; ++x) {
            int sx = static_cast<int>(x / scale);
            if (sx >= src_w)
                sx = src_w - 1;
            float b = src_row[sx * 3 + 0] / 255.0f;
            float g = src_row[sx * 3 + 1] / 255.0f;
            float r = src_row[sx * 3 + 2] / 255.0f;
            int di = dy * dst_w + (x + pad_x);
            dst_r[di] = r;
            dst_g[di] = g;
            dst_b[di] = b;
        }
    }
}

} // namespace

PreprocessResult Preprocessor::run(const SsvVideoFrame &frame, const TensorSpec &input_spec) const
{
    if (input_spec.shape.size() != 4)
        throw std::invalid_argument("only 4D NCHW image inputs are supported");
    if (input_spec.shape[0] != 1 || input_spec.shape[1] != 3)
        throw std::invalid_argument("only batch=1, channels=3 image inputs are supported");
    if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 || frame.bgr.empty())
        throw std::invalid_argument("invalid video frame for preprocessing");

    int model_h = checked_dim(input_spec, 2, "height");
    int model_w = checked_dim(input_spec, 3, "width");
    size_t tensor_size = 3 * static_cast<size_t>(model_w) * static_cast<size_t>(model_h);

    PreprocessResult result;
    result.original_width = frame.width;
    result.original_height = frame.height;
    result.model_width = model_w;
    result.model_height = model_h;
    result.input.spec = input_spec;
    result.input.host_data.resize(tensor_size);

    preprocess_bgr_to_chw(frame.bgr.data(), frame.width, frame.height, frame.stride,
                          result.input.host_data.data(), model_w, model_h);
    return result;
}

} // namespace ssv::infer
