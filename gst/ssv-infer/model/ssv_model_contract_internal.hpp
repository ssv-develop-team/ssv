#pragma once

#include "public/ssv_model_contract.hpp"
#include "core/ssv_tensor.hpp"

#include <stdexcept>
#include <cstddef>
#include <string>

namespace ssv::infer {

struct SsvModelInputContract {
    TensorSpec input;
    std::size_t element_count = 0;
    std::size_t byte_size = 0;

    [[nodiscard]] std::string description() const;
};

class SsvModelContractError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

[[nodiscard]] SsvModelContract ssv_model_contract_validate(
    const ModelMetadata &metadata,
    ModelFamily model_family,
    OutputFormat output_format,
    ssv::SsvResizeMode resize_mode);

[[nodiscard]] SsvModelInputContract ssv_model_input_contract_validate(
    const ModelMetadata &metadata,
    ModelFamily model_family,
    OutputFormat output_format);

} // namespace ssv::infer
