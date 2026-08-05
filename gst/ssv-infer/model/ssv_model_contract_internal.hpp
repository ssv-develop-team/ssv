#pragma once

#include "public/ssv_model_contract.hpp"
#include "core/ssv_tensor.hpp"

#include <stdexcept>

namespace ssv::infer {

class SsvModelContractError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

[[nodiscard]] SsvModelContract ssv_model_contract_validate(
    const ModelMetadata &metadata,
    ModelFamily model_family,
    OutputFormat output_format);

} // namespace ssv::infer
