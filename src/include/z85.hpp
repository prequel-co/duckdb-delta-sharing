#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace duckdb {

class Z85 {
public:
    static std::vector<uint8_t> Decode(const std::string &input) {
        if (input.empty()) {
            return {};
        }

        if (input.length() % 5 != 0) {
            throw std::runtime_error("Z85: input length must be a multiple of 5");
        }

        size_t decoded_size = (input.length() / 5) * 4;
        std::vector<uint8_t> output(decoded_size);

        static const char *encoder =
            "0123456789"
            "abcdefghij"
            "klmnopqrst"
            "uvwxyzABCD"
            "EFGHIJKLMN"
            "OPQRSTUVWX"
            "YZ.-:+=^/*"
            "?&<>()[]{}"
            "@%$#";

        static uint8_t decoder[256];
        static bool initialized = false;
        if (!initialized) {
            for (int i = 0; i < 256; i++) decoder[i] = 0xFF;
            for (uint8_t i = 0; i < 85; i++) decoder[(uint8_t)encoder[i]] = i;
            initialized = true;
        }

        uint32_t value = 0;
        size_t byte_idx = 0;
        for (size_t i = 0; i < input.length(); i++) {
            uint8_t char_val = decoder[(uint8_t)input[i]];
            if (char_val == 0xFF) {
                throw std::runtime_error("Z85: invalid character in input");
            }

            value = value * 85 + char_val;
            if ((i + 1) % 5 == 0) {
                output[byte_idx++] = (uint8_t)((value >> 24) & 0xFF);
                output[byte_idx++] = (uint8_t)((value >> 16) & 0xFF);
                output[byte_idx++] = (uint8_t)((value >> 8) & 0xFF);
                output[byte_idx++] = (uint8_t)(value & 0xFF);
                value = 0;
            }
        }

        return output;
    }
};

} // namespace duckdb
