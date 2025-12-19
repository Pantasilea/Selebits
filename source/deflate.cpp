#include "deflate.hpp"

#include <algorithm>

std::vector<std::uint8_t> sel::decompress_deflate(std::span<const std::uint8_t> deflate_data)
{
    impl::Bitstream bitstream {deflate_data};
    std::uint32_t bfinal {bitstream.peek_bits(1)};
    if(bfinal) throw Exception {Error::bad_formed_data};

    std::vector<std::uint8_t> inflated_data;
    inflated_data.reserve(5000); // 5KB

    do {
        bfinal = bitstream.read_bits(1);
        const std::uint32_t btype {bitstream.read_bits(2)};
        switch(btype) {
            case 0: // no compression
                bitstream.skip_until_next_byte_boundary();
                impl::deflate::decompress_uncompressed(inflated_data, bitstream);
                break;
            case 1: // fixed Huffman codes
                impl::deflate::decompress_fixed(inflated_data, bitstream);
                break;
            case 2: // dynamic Huffman codes
                impl::deflate::decompress_dynamic(inflated_data, bitstream);
                break;
            default:
                throw Exception {Error::bad_formed_data};
        }
    } while(not bfinal);

    return inflated_data;
}

void sel::impl::deflate::decompress_uncompressed(std::vector<std::uint8_t>& inflated_data, Bitstream& bitstream)
{
    std::uint32_t len {bitstream.read_bits(16)};
    std::uint32_t nlen {bitstream.read_bits(16)};
    if(~len != nlen) throw Exception {Error::bad_formed_data};
    if(len == 0u) return; // zero length is allowed

    std::span<const std::uint8_t> uncompressed_data {bitstream.read_bytes(len)};
    inflated_data.insert(inflated_data.end(), uncompressed_data.begin(), uncompressed_data.end());
}

void sel::impl::deflate::decompress_fixed(std::vector<std::uint8_t>& inflated_data, Bitstream& bitstream)
{
    static const std::vector<Huffman_code> huffman_codes(make_fixed_huffman_table());
    std::uint32_t symbol {fetch_symbol_in_fixed_block(huffman_codes, bitstream)};
    while(symbol != 256u) {
        if(symbol < 256u) {
            inflated_data.push_back(static_cast<std::uint8_t>(symbol));
        }
        else {
            if(symbol > 285u) throw Exception {Error::bad_formed_data};
            symbol -= 257u;
            const std::uint32_t length {length_bases[symbol] + bitstream.read_bits(length_extra_bits[symbol])};
            if(length > 258u) throw Exception {Error::bad_formed_data};

            // for fixed huffman blocks, a distance huffman code and its symbol have the same values
            symbol = bitstream.read_bits(5);
            symbol = bitswap_from_lsbit(symbol, 5);
            if(symbol > 29u) throw Exception {Error::bad_formed_data};
            const std::uint32_t distance {distance_bases[symbol] + bitstream.read_bits(distance_extra_bits[symbol])};
            if(distance > inflated_data.size() or distance > 32768u) throw Exception {Error::bad_formed_data};

            lz77_copy(inflated_data, length, distance);
        }

        symbol = fetch_symbol_in_fixed_block(huffman_codes, bitstream);
    }
}

void sel::impl::deflate::decompress_dynamic(std::vector<std::uint8_t>& inflated_data, Bitstream& bitstream)
{
    const std::uint32_t hlit {bitstream.read_bits(5) + 257u};
    const std::uint32_t hdist {bitstream.read_bits(5) + 1u};
    const std::uint32_t hclen {bitstream.read_bits(4) + 4u};
    if(hlit > 286u or hdist > 30u) throw Exception {Error::bad_formed_data};

    // the order of slots with which to place the bit-lengths of the codes of the code bit-length alphabet
    static constexpr std::array<int, 19> ordered_indexes {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    std::array<std::uint32_t, 19> code_length_alphabet_bit_lengths;
    for(std::uint32_t i = 0u; i < hclen; ++i) {
        code_length_alphabet_bit_lengths[ordered_indexes[i]] = bitstream.read_bits(3);
    }
    for(std::uint32_t i = hclen; i < 19u; ++i) {
        code_length_alphabet_bit_lengths[ordered_indexes[i]] = 0u;
    }
    std::vector<Huffman_code> code_length_alphabet(make_huffman_codes_from_bit_lengths(code_length_alphabet_bit_lengths));

    // bit-lengths of both the literal+length alphabet and the distance alphabet
    std::vector<std::uint32_t> alphabets_bit_lengths;
    const std::uint32_t hlit_hdist {hlit + hdist};
    alphabets_bit_lengths.reserve(hlit_hdist);
    // for(std::uint32_t i = 0u; i < hlit_hdist; ++i) <- Cannot be like this
    while(alphabets_bit_lengths.size() < hlit_hdist) {
        const std::uint32_t symbol {fetch_symbol_in_dynamic_block(code_length_alphabet, bitstream)};
        if(symbol < 16u) {
            alphabets_bit_lengths.push_back(symbol);
        }
        else if(symbol == 16u) {
            if(alphabets_bit_lengths.empty()) throw Exception {Error::bad_formed_data};
            const std::uint32_t value_to_copy {alphabets_bit_lengths.back()};
            const std::uint32_t times_to_copy {bitstream.read_bits(2) + 3u};
            alphabets_bit_lengths.insert(alphabets_bit_lengths.end(), times_to_copy, value_to_copy);
        }
        else if(symbol == 17u) {
            const std::uint32_t times_to_copy {bitstream.read_bits(3) + 3u};
            alphabets_bit_lengths.insert(alphabets_bit_lengths.end(), times_to_copy, 0u);
        }
        else if(symbol == 18u) {
            const std::uint32_t times_to_copy {bitstream.read_bits(7) + 11u};
            alphabets_bit_lengths.insert(alphabets_bit_lengths.end(), times_to_copy, 0u);
        }
        else { throw Exception {Error::bad_formed_data}; }
    }

    std::span<const std::uint32_t> literal_length_alphabet_bit_lengths(alphabets_bit_lengths.begin(), hlit);
    std::vector<Huffman_code> literal_length_alphabet(make_huffman_codes_from_bit_lengths(literal_length_alphabet_bit_lengths));
    /* this is so silly: the case in where the amount of bit-lengths for the distance alphabet is 1
    * and that lonely bit-length happens to be zero is valid, it means that the data to decompress
    * is all literals and there aren't length or distance codes. It's silly because the "no compression"
    * block (BTYPE == 0) already exists, this was completely unnecessary.
    */
    std::span<const std::uint32_t> distance_alphabet_bit_lengths(alphabets_bit_lengths.begin() + hlit, hdist);
    std::vector<Huffman_code> distance_alphabet;
    if(distance_alphabet_bit_lengths.size() == 1u and distance_alphabet_bit_lengths[0] == 0u) { /* do nothing */ }
    else { distance_alphabet = make_huffman_codes_from_bit_lengths(distance_alphabet_bit_lengths); }

    /* loop copy-pasted from decompress_fixed, the differences are small and I could put this loop
    * in a single function to avoid code duplication, but in this case, I am going to allow the
    * duplication */
    std::uint32_t symbol {fetch_symbol_in_dynamic_block(literal_length_alphabet, bitstream)};
    while(symbol != 256u) {
        if(symbol < 256u) {
            inflated_data.push_back(static_cast<std::uint8_t>(symbol));
        }
        else {
            if(distance_alphabet.empty()) throw Exception {Error::bad_formed_data};
            if(symbol > 285u) throw Exception {Error::bad_formed_data};
            symbol -= 257u;
            const std::uint32_t length {length_bases[symbol] + bitstream.read_bits(length_extra_bits[symbol])};
            if(length > 258u) throw Exception {Error::bad_formed_data};

            symbol = fetch_symbol_in_dynamic_block(distance_alphabet, bitstream);
            if(symbol > 29u) throw Exception {Error::bad_formed_data};
            const std::uint32_t distance {distance_bases[symbol] + bitstream.read_bits(distance_extra_bits[symbol])};
            if(distance > inflated_data.size() or distance > 32768u) throw Exception {Error::bad_formed_data};

            lz77_copy(inflated_data, length, distance);
        }

        symbol = fetch_symbol_in_dynamic_block(literal_length_alphabet, bitstream);
    }
}

std::vector<sel::impl::deflate::Huffman_code> sel::impl::deflate::make_fixed_huffman_table()
{
    // bl_count[7 (for example)] == number of codes that have 7 bits
    /*
    constexpr std::array<std::uint32_t, 10> bl_count {
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 24u, 152u, 112u
    };
    */

    std::vector<std::uint32_t> bit_lengths;
    bit_lengths.reserve(288);
    for(int i = 0; i < 144; ++i) { bit_lengths.push_back(8u); }
    for(int i = 144; i < 256; ++i) { bit_lengths.push_back(9u); }
    for(int i = 256; i < 280; ++i) { bit_lengths.push_back(7u); }
    for(int i = 280; i < 288; ++i) { bit_lengths.push_back(8u); }

    // generate the codes for each bit-length
    std::vector<Huffman_code> huffman_codes;
    huffman_codes.reserve(288);
    std::uint32_t code {0u}; // the smallest valid code
    for(std::uint32_t i = 7; i < 10; ++i) {
        for(std::size_t j = 0u; j < bit_lengths.size(); ++j) {
            if(bit_lengths[j] != i) continue;

            huffman_codes.emplace_back(code, i, static_cast<std::uint32_t>(j));
            // within a bit-length, the codes are assigned consecutive values
            ++code;
        }

        // an extra bit must be added just before going to the next bit-length
        code <<= 1u;
    }

    return huffman_codes;
}

std::uint32_t sel::impl::deflate::fetch_symbol_in_fixed_block(const std::vector<Huffman_code>& huffman_codes, Bitstream& bitstream)
{
    for(int i = 7; i < 10; ++i) {
        std::uint32_t code {bitstream.peek_bits(i)};
        code = bitswap_from_lsbit(code, i);

        for(const Huffman_code& hc : huffman_codes) {
            if(hc.code == code and hc.bit_length == i) {
                bitstream.skip_bits(i);
                return hc.symbol;
            }
        }
    }

    throw sel::Exception {sel::Error::bad_formed_data};
}

std::vector<sel::impl::deflate::Huffman_code> sel::impl::deflate::make_huffman_codes_from_bit_lengths(std::span<const std::uint32_t> bit_lengths)
{
    auto iter {std::max_element(bit_lengths.begin(), bit_lengths.end())};
    // bl_count[7 (for example)] == number of codes that have 7 bits
    std::vector<std::uint32_t> bl_count;
    bl_count.resize(*iter + 1u);

    // count the number of codes for each bit-length
    for(std::size_t i = 0u; i < bit_lengths.size(); ++i) {
        bl_count[bit_lengths[i]] += 1u;
    }

    // generate the codes for each bit-length
    std::vector<Huffman_code> huffman_codes;
    std::uint32_t code {0u}; // the smallest valid code
    for(std::uint32_t i = 1; i <= *iter; ++i) {
        // an extra bit must be added just before going to the next bit-length
        code <<= 1u;
        /* the above "code <<= 1u;" must be before the below
        * if(bl_count[i] == 0u) continue;" because there can be bl_counts that
        * are equal to zero between bl_counts that aren't, and the left-shift
        * must still be done
        */
        if(bl_count[i] == 0u) continue;

        for(std::size_t j = 0u; j < bit_lengths.size(); ++j) {
            if(bit_lengths[j] != i) continue;

            huffman_codes.emplace_back(code, i, static_cast<std::uint32_t>(j));
            // within a bit-length, the codes are assigned consecutive values
            ++code;
        }
    }

    return huffman_codes;
}

std::uint32_t sel::impl::deflate::fetch_symbol_in_dynamic_block(const std::vector<Huffman_code>& huffman_codes, Bitstream& bitstream)
{
    for(std::uint32_t i = 1u; i < 16u; ++i) {
        std::uint32_t code {bitstream.peek_bits(i)};
        code = bitswap_from_lsbit(code, i);

        for(const Huffman_code& hc : huffman_codes) {
            if(hc.code == code and hc.bit_length == i) {
                bitstream.skip_bits(i);
                return hc.symbol;
            }
        }
    }

    throw Exception {Error::bad_formed_data};
}

void sel::impl::deflate::lz77_copy(std::vector<std::uint8_t>& inflated_data, const std::uint32_t length, const std::uint32_t distance)
{
    const std::size_t beginning_of_copy {inflated_data.size() - distance};
    std::size_t copy_from {beginning_of_copy};

    inflated_data.reserve(inflated_data.size() + length);
    for(std::uint32_t i = 0u; i < length; ++i) {
        const std::uint8_t value_to_copy {inflated_data[copy_from]};
        inflated_data.push_back(value_to_copy);
        ++copy_from;
        if(copy_from == inflated_data.size()) {
            copy_from = beginning_of_copy;
        }
    }
}
