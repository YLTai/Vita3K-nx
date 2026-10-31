template <typename T>
static void convert_uniform_data(std::vector<std::uint8_t> &converted_data, const float *sourceData, uint32_t componentCount) {
    converted_data.resize(componentCount * sizeof(T));
    for (std::uint32_t i = 0; i < componentCount; ++i) {
        T converted = static_cast<T>(sourceData[i]);
        std::memcpy(&converted_data[i * sizeof(T)], &converted, sizeof(T));
    }
}

EXPORT(int, sceGxmSetUniformDataF, void *uniformBuffer, const SceGxmProgramParameter *parameter, uint32_t componentOffset, uint32_t componentCount, const float *sourceData) {
    TRACY_FUNC(sceGxmSetUniformDataF, uniformBuffer, parameter, componentOffset, componentCount, sourceData);
    assert(parameter);

    if (!uniformBuffer || !parameter || !sourceData)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_POINTER);

    if (parameter->category != SceGxmParameterCategory::SCE_GXM_PARAMETER_CATEGORY_UNIFORM)
        return RET_ERROR(SCE_GXM_ERROR_INVALID_VALUE);

    size_t size = 0;
    size_t offset = 0;
    bool is_float = false;

    const std::uint16_t param_type = parameter->type;

    // Component size is in bytes
    int comp_size = gxp::get_parameter_type_size(static_cast<SceGxmParameterType>(param_type));
    const std::uint8_t *source = reinterpret_cast<const std::uint8_t *>(sourceData);
    std::vector<std::uint8_t> converted_data;

    switch (parameter->type) {
    case SCE_GXM_PARAMETER_TYPE_S8: {
        convert_uniform_data<int8_t>(converted_data, sourceData, componentCount);
        source = converted_data.data();
        break;
    }

    case SCE_GXM_PARAMETER_TYPE_U8: {
        convert_uniform_data<uint8_t>(converted_data, sourceData, componentCount);
        source = converted_data.data();
        break;
    }

    case SCE_GXM_PARAMETER_TYPE_U16: {
        convert_uniform_data<uint16_t>(converted_data, sourceData, componentCount);
        source = converted_data.data();
        break;
    }

    case SCE_GXM_PARAMETER_TYPE_S16: {
        convert_uniform_data<int16_t>(converted_data, sourceData, componentCount);
        source = converted_data.data();
        break;
    }

    case SCE_GXM_PARAMETER_TYPE_U32: {
        convert_uniform_data<uint32_t>(converted_data, sourceData, componentCount);
        source = converted_data.data();
        break;
    }

    case SCE_GXM_PARAMETER_TYPE_S32: {
        convert_uniform_data<int32_t>(converted_data, sourceData, componentCount);
        source = converted_data.data();
        break;
    }

    case SCE_GXM_PARAMETER_TYPE_F16: {
        converted_data.resize(((componentCount + 7) / 8) * 8 * 2);
        float_to_half(sourceData, reinterpret_cast<std::uint16_t *>(converted_data.data()), componentCount);

        source = converted_data.data();
        is_float = true;
        break;
    }

    case SCE_GXM_PARAMETER_TYPE_F32: {
        is_float = true;
        break;
    }
    default:
        assert(false);
    }

    if (parameter->array_size == 1 || parameter->component_count == 1) {
        // Case 1: No array. Only a single vector. Don't apply any alignment
        // Case 2: Array but component count equals to 1. This case, a scalar array, align it to 32-bit bound
        if (parameter->component_count == 1) {
            // Apply 32 bit alignment, by making each component has 4 bytes
            comp_size = 4;
        }

        size = componentCount * comp_size;
        offset = parameter->resource_index * sizeof(float) + componentOffset * comp_size;

        memcpy(static_cast<uint8_t *>(uniformBuffer) + offset, source, size);
    } else {
        // This is the size of each element.
        size = parameter->component_count * comp_size;
        int align_bytes = 0;

        if (is_float) {
            // Align it to 64-bit boundary (8 bytes)
            if ((size & 7) != 0) {
                align_bytes = 8 - (size & 7);
            }
        } else {
            // Align it to 32-bit boundary (4 bytes)
            if ((size & 3) != 0) {
                align_bytes = 4 - (size & 3);
            }
        }

        // wtf
        // wtf
        const int vec_to_start_write = componentOffset / parameter->component_count;
        int component_cursor_inside_vector = (componentOffset % parameter->component_count);
        std::uint8_t *dest = static_cast<uint8_t *>(uniformBuffer) + parameter->resource_index * sizeof(float)
            + vec_to_start_write * (size + align_bytes) + component_cursor_inside_vector * comp_size;

        int component_to_copy_remain_per_elem = parameter->component_count - component_cursor_inside_vector;
        int component_left_to_copy = componentCount;

        while (component_left_to_copy > 0) {
            memcpy(dest, source, component_to_copy_remain_per_elem * comp_size);

            // Add and align destination
            dest += comp_size * component_to_copy_remain_per_elem + align_bytes;
            source += component_to_copy_remain_per_elem * comp_size;

            component_left_to_copy -= component_to_copy_remain_per_elem;
            component_to_copy_remain_per_elem = std::min<int>(4, component_to_copy_remain_per_elem);
        }
    }

    return 0;
}
