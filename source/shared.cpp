#include "shared.hpp"
#include <sstream>

sel::Exception::Exception(Error error, std::source_location sl)
    : m_error {error}, m_source_location {sl}
{
    std::ostringstream ostr;
    ostr << "SELEBITS-EXCEPTION REPORT:\n"
        << "Function name: " << sl.function_name() << '\n'
        << "File name: " << sl.file_name() << '\n'
        << "Line: " << sl.line() << '\n'
        << "Column: " << sl.column() << '\n';

    m_message = ostr.str();
}

std::span<const std::uint8_t> sel::impl::Bytestream::get_bytes(const std::size_t amount)
{
    if(m_current_byte_index > (m_source.size() - 1u)) {
        throw Exception {Error::bad_formed_data};
    }

    if(m_current_byte_index + amount > m_source.size()) {
        throw Exception {Error::bad_formed_data};
    }

    std::span<const std::uint8_t> result(m_source.begin() + m_current_byte_index, amount);
    m_current_byte_index += amount;
    return result;
}

std::uint32_t sel::impl::Bitstream::read_bits(const std::uint32_t amount)
{
    if(amount == 0u) return 0u;
    
    if(m_current_byte_index > (m_source.size() - 1u)) {
        throw sel::Exception {sel::Error::bad_formed_data};
    }

    std::uint32_t bits_taken {0u};
    std::uint32_t result {0u};

    while(bits_taken < amount) {
        std::uint32_t bits_to_take {amount - bits_taken};
        if(bits_to_take > m_useful_bits_in_current_byte) { bits_to_take = m_useful_bits_in_current_byte; }

        // to move outside/discard the bits that are not useful anymore
        const std::uint32_t discard_rotation {8u - m_useful_bits_in_current_byte};

        const std::uint32_t mask = (1u << bits_to_take) - 1u;
        std::uint32_t temp {(m_source[m_current_byte_index] >> discard_rotation) & mask};
        temp <<= bits_taken;
        result |= temp;

        // book-keep
        bits_taken += bits_to_take;
        m_useful_bits_in_current_byte -= bits_to_take;

        if(m_useful_bits_in_current_byte == 0u) {
            // go to the next byte
            ++m_current_byte_index;
            if(m_current_byte_index > (m_source.size() - 1u)) {
                throw sel::Exception {sel::Error::bad_formed_data};
            }
            m_useful_bits_in_current_byte = 8u;
        }
    }

    return result;
}

std::uint32_t sel::impl::Bitstream::peek_bits(const std::uint32_t amount)
{
    if(amount == 0u) return 0u;

    const std::size_t copy1 {m_current_byte_index};
    const std::uint32_t copy2 {m_useful_bits_in_current_byte};

    const std::uint32_t result {read_bits(amount)};
    m_current_byte_index = copy1;
    m_useful_bits_in_current_byte = copy2;

    return result;
}

void sel::impl::Bitstream::skip_bits(const std::uint32_t amount)
{
    if(amount == 0u) return;

    if(m_current_byte_index > (m_source.size() - 1u)) {
        throw sel::Exception {sel::Error::bad_formed_data};
    }

    std::uint32_t bits_skipped {0};
    while(bits_skipped < amount) {
        std::uint32_t bits_to_skip {amount - bits_skipped};
        if(bits_to_skip > m_useful_bits_in_current_byte) { bits_to_skip = m_useful_bits_in_current_byte; }

        // book-keep
        bits_skipped += bits_to_skip;
        m_useful_bits_in_current_byte -= bits_to_skip;

        if(m_useful_bits_in_current_byte == 0u) {
            // go to the next byte
            ++m_current_byte_index;
            if(m_current_byte_index > (m_source.size() - 1u)) {
                throw sel::Exception {sel::Error::bad_formed_data};
            }
            m_useful_bits_in_current_byte = 8u;
        }
    }
}

void sel::impl::Bitstream::skip_until_next_byte_boundary()
{
    if(m_useful_bits_in_current_byte == 8u) return;
    
    ++m_current_byte_index;
    if(m_current_byte_index > (m_source.size() - 1u)) {
        throw sel::Exception {sel::Error::bad_formed_data};
    }
    m_useful_bits_in_current_byte = 8u;
}

std::span<const std::uint8_t> sel::impl::Bitstream::read_bytes(const std::uint32_t amount)
{
    if(amount == 0u) return {};

    if(m_current_byte_index + amount > m_source.size()) {
        throw sel::Exception {sel::Error::bad_formed_data};
    }

    std::span<const std::uint8_t> result {m_source.begin() + m_current_byte_index, amount};

    // book-keeping
    m_current_byte_index += amount;
    if(m_current_byte_index > m_source.size() - 1u) {
        m_useful_bits_in_current_byte = 0u;
    }
    else { m_useful_bits_in_current_byte = 8u; }

    return result;
}