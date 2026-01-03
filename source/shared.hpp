#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <source_location>
#include <type_traits>
#include <concepts>
#include <span>
#include <bit>
#include <cstring>

namespace sel {
    enum class Error {
        none,
        bug,
        bad_formed_data,
        unexpected_eof
    };

    class Exception : public std::exception {
    public:
        Exception(Error error, std::source_location sl = std::source_location::current());
        
        const char* what() const noexcept final { return m_message.c_str(); }
        Error error() const noexcept { return m_error; }
        std::source_location source_location() const noexcept { return m_source_location; }
    private:
        std::string m_message;
        std::source_location m_source_location;
        Error m_error;
    };
}

namespace sel::impl {
    template<std::integral T>
    constexpr T byteswap(const T value) noexcept
    {
        if constexpr(sizeof(T) == 1u) return value;

        const std::make_unsigned_t<T> temp {static_cast<std::make_unsigned_t<T>>(value)};
        std::make_unsigned_t<T> result {};

        constexpr std::size_t bytes_in_type {sizeof(T)};
        for(std::size_t i = 0u; i < bytes_in_type; ++i) {
            result |= ((temp >> (8u * i)) & 0xFFu) << (8u * (bytes_in_type - 1u - i));
        }

        return static_cast<T>(result);
    }

    template<std::integral T>
    constexpr T bitswap_from_lsbit(const T value, const std::uint32_t amount)
    {
        if(amount < 2u) return value;
        if(amount > sizeof(T) * 8u) throw sel::Exception {sel::Error::bug};

        const std::make_unsigned_t<T> temp {static_cast<std::make_unsigned_t<T>>(value)};
        constexpr std::make_unsigned_t<T> one {1u};

        std::make_unsigned_t<T> result {};
        for(std::uint32_t i = 0; i < amount; ++i) {
            result |= ((temp & (one << i)) >> i) << ((amount - 1u) - i);
        }

        return static_cast<T>(result);
    }

    class Bytestream {
    public:
        Bytestream(std::span<const std::uint8_t> source) noexcept : m_source {source} {}

        template<std::integral T>
        T get_from_little_endian();

        template<std::integral T>
        T get_from_big_endian();

        std::span<const std::uint8_t> get_bytes(const std::size_t amount);
    private:
        template<std::integral T>
        T plain_get();

        std::span<const std::uint8_t> m_source;
        std::size_t m_current_byte_index {0u};
    };

    template<std::integral T>
    T Bytestream::plain_get()
    {
        if(m_current_byte_index > (m_source.size() - 1u)) {
            throw Exception {Error::bad_formed_data};
        }

        if(m_current_byte_index + sizeof(T) > m_source.size()) {
            throw Exception {Error::bad_formed_data};
        }

        std::uint8_t bytes[sizeof(T)];
        for(std::size_t i = 0u; i < sizeof(T); ++i) {
            bytes[i] = m_source[m_current_byte_index];
            ++m_current_byte_index;
        }

        T result;
        std::memcpy(&result, bytes, sizeof(T));

        return result;
    }

    template<std::integral T>
    T Bytestream::get_from_little_endian()
    {
        T result {plain_get<T>()};
        if constexpr(std::endian::native != std::endian::little) {
            result = byteswap(result);
        }

        return result;
    }

    template<std::integral T>
    T Bytestream::get_from_big_endian()
    {
        T result {plain_get<T>()};
        if constexpr(std::endian::native != std::endian::big) {
            result = byteswap(result);
        }

        return result;
    }

    enum class Bitstream_format {
        /* in the examples, 'a' is a 10 bits value and 'b' is a 6 bits value.
        * 'a' is the first value in the bit-stream and 'b' is the second.
        * the numbers inside the parenthesis indicate the significant bits
        * of the values, 0 is the less significant bit. */
        gif, // byte 0: aaaaaaaa (76543210), byte 1: bbbbbbaa (54321098)
        jpg // byte 0: aaaaaaaa (98765432), byte 1: aabbbbbb (10543210)
    };

    template<Bitstream_format format>
    class Bitstream {
    public:
        Bitstream(std::span<const std::uint8_t> source) noexcept : m_source {source} {}

        std::uint32_t read_bits(const std::uint32_t amount);
        std::uint32_t peek_bits(const std::uint32_t amount);
        void skip_bits(const std::uint32_t amount);
        void skip_until_next_byte_boundary();

        std::span<const std::uint8_t> read_bytes(const std::uint32_t amount);
    private:
        // [verb]_bits_FORMAT
        //std::uint32_t read_bits_lsbit(const std::uint32_t amount);

        std::span<const std::uint8_t> m_source;
        std::size_t m_current_byte_index {0u};
        std::uint32_t m_useful_bits_in_current_byte {8u};
    };
}
