#include "deflate.hpp"

#include <algorithm>

constexpr std::strong_ordering sel::impl::deflate::Huffman_code::operator<=>(const Huffman_code& rhs) const
{
    if(code < rhs.code) { return std::strong_ordering::less; }
    else if(code > rhs.code) { return std::strong_ordering::greater; }
    return std::strong_ordering::equal;
}

std::vector<std::uint8_t> sel::decompress_deflate(std::span<const std::uint8_t> deflate_data)
{
    impl::Bitstream bitstream {deflate_data};
    std::uint32_t bfinal {bitstream.read_bits(1)};
    if(bfinal) throw Exception {Error::bad_formed_data};

    std::vector<std::uint8_t> inflated_data;
    inflated_data.reserve(5000); // 5KB
    bool must_continue {true};
    while(not bfinal or must_continue) {
        if(bfinal) must_continue = false;

        std::uint32_t btype {bitstream.read_bits(2)};
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

        if(must_continue) bfinal = bitstream.read_bits(1);
    }

    return inflated_data;
}

void sel::impl::deflate::decompress_uncompressed(std::vector<std::uint8_t>& inflated_data, Bitstream& bitstream)
{
    std::uint32_t len {bitstream.read_bits(16)};
    std::uint32_t nlen {bitstream.read_bits(16)};
    if(~len != nlen) throw Exception {Error::bad_formed_data};
    if(len == 0u) return;

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
            std::uint32_t length {length_bases[symbol]};
            const std::uint32_t length_extra {bitstream.read_bits(length_extra_bits[symbol])};
            length += length_extra;
            if(length > 258u) throw Exception {Error::bad_formed_data};

            // for fixed huffman blocks, a distance huffman code and its symbol have the same values
            symbol = bitstream.read_bits(5);
            symbol = bitswap_from_lsbit(symbol, 5);
            if(symbol > 29u) throw Exception {Error::bad_formed_data};
            std::uint32_t distance {distance_bases[symbol]};
            const std::uint32_t distance_extra {bitstream.read_bits(distance_extra_bits[symbol])};
            distance += distance_extra;
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
    for(std::uint32_t i = 0u; i < hlit_hdist; ++i) {
        const std::uint32_t symbol {fetch_symbol_in_dynamic_block(code_length_alphabet, bitstream)};
        if(symbol < 16u) {
            alphabets_bit_lengths.push_back(symbol);
        }
        else if(symbol == 16u) {
            if(alphabets_bit_lengths.empty()) throw Exception {Error::bad_formed_data};
            const std::uint32_t value_to_copy {alphabets_bit_lengths.back()};
            const std::uint32_t times_to_copy {bitstream.read_bits(2) + 3u};
            if(alphabets_bit_lengths.size() + times_to_copy > hlit_hdist) throw Exception {Error::bad_formed_data};
            alphabets_bit_lengths.insert(alphabets_bit_lengths.end(), times_to_copy, value_to_copy);
        }
        else if(symbol == 17u) {
            const std::uint32_t times_to_copy {bitstream.read_bits(3) + 3u};
            if(alphabets_bit_lengths.size() + times_to_copy > hlit_hdist) throw Exception {Error::bad_formed_data};
            alphabets_bit_lengths.insert(alphabets_bit_lengths.end(), times_to_copy, 0u);
        }
        else if(symbol == 18u) {
            const std::uint32_t times_to_copy {bitstream.read_bits(7) + 11u};
            if(alphabets_bit_lengths.size() + times_to_copy > hlit_hdist) throw Exception {Error::bad_formed_data};
            alphabets_bit_lengths.insert(alphabets_bit_lengths.end(), times_to_copy, 0u);
        }
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
            std::uint32_t length {length_bases[symbol]};
            const std::uint32_t length_extra {bitstream.read_bits(length_extra_bits[symbol])};
            length += length_extra;
            if(length > 258u) throw Exception {Error::bad_formed_data};

            symbol = fetch_symbol_in_dynamic_block(distance_alphabet, bitstream);
            if(symbol > 29u) throw Exception {Error::bad_formed_data};
            std::uint32_t distance {distance_bases[symbol]};
            const std::uint32_t distance_extra {bitstream.read_bits(distance_extra_bits[symbol])};
            distance += distance_extra;
            if(distance > inflated_data.size() or distance > 32768u) throw Exception {Error::bad_formed_data};

            lz77_copy(inflated_data, length, distance);
        }

        symbol = fetch_symbol_in_dynamic_block(literal_length_alphabet, bitstream);
    }
}

std::vector<sel::impl::deflate::Huffman_code> sel::impl::deflate::make_fixed_huffman_table()
{
    // bl_count[7 (for example)] == number of codes that have 7 bits
    constexpr std::array<std::uint32_t, 10> bl_count {
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 24u, 152u, 112u
    };

    std::vector<Huffman_code> huffman_codes;
    huffman_codes.resize(288);
    for(int i = 0; i < 144; ++i) { huffman_codes[i].bit_length = 8u; }
    for(int i = 144; i < 256; ++i) { huffman_codes[i].bit_length = 9u; }
    for(int i = 256; i < 280; ++i) { huffman_codes[i].bit_length = 7u; }
    for(int i = 280; i < 288; ++i) { huffman_codes[i].bit_length = 8u; }

    std::array<std::uint32_t, 10> smallest_codes; // smallest codes per bit-lengths
    std::uint32_t minimum_code {0u};
    // find the smallest code per bit-length
    for(int i = 1; i <= 9; ++i) {
        minimum_code = (minimum_code + bl_count[i - 1]) << 1u;
        smallest_codes[i] = minimum_code;
    }

    // assign consecutive values to all the codes that have the same bit-length
    for(int i = 0; i < 288; ++i) {
        const std::uint32_t bit_length {huffman_codes[i].bit_length};

        huffman_codes[i].code = smallest_codes[bit_length];
        smallest_codes[bit_length] += 1;

        huffman_codes[i].symbol = i;
    }

    // sort to be able to do binary searches
    std::sort(huffman_codes.begin(), huffman_codes.end());
    return huffman_codes;
}

std::uint32_t sel::impl::deflate::fetch_symbol_in_fixed_block(const std::vector<Huffman_code>& huffman_codes, Bitstream& bitstream)
{
    for(int i = 7; i < 10; ++i) {
        std::uint32_t code {bitstream.peek_bits(i)};
        code = bitswap_from_lsbit(code, i);
        auto iter {std::lower_bound(huffman_codes.begin(), huffman_codes.end(), code, [](const Huffman_code hc, const std::uint32_t value) { return hc.code < value; })};

        if(iter != huffman_codes.end() and iter->code == code) {
            bitstream.skip_bits(i);
            return iter->symbol;
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

    std::vector<std::uint32_t> smallest_codes; // smallest codes per bit-lengths
    smallest_codes.resize(bl_count.size());
    std::uint32_t minimum_code {0u};
    // find the smallest code per bit-length
    for(std::size_t i = 1u; i <= smallest_codes.size() - 1u; ++i) {
        minimum_code = (minimum_code + bl_count[i - 1u]) << 1u;
        smallest_codes[i] = minimum_code;
    }

    // assign consecutive values to all the codes that have the same bit-length
    std::vector<Huffman_code> huffman_codes;
    huffman_codes.reserve(bit_lengths.size() - bl_count[0]); // codes with bit-lengths of zero are invalid
    for(std::size_t i = 0u; i < bit_lengths.size(); ++i) {
        const std::uint32_t bit_length {bit_lengths[i]};
        if(bit_length != 0u) {
            std::uint32_t code = smallest_codes[bit_length];
            smallest_codes[bit_length] += 1;

            huffman_codes.emplace_back(code, bit_length, static_cast<std::uint32_t>(i));
        }
    }

    // sort to be able to do binary searches
    std::sort(huffman_codes.begin(), huffman_codes.end());
    return huffman_codes;
}

std::uint32_t sel::impl::deflate::fetch_symbol_in_dynamic_block(const std::vector<Huffman_code>& huffman_codes, Bitstream& bitstream)
{
    for(std::uint32_t i = 1u; i < 16u; ++i) {
        std::uint32_t code {bitstream.peek_bits(i)};
        code = bitswap_from_lsbit(code, i);
        auto iter {std::lower_bound(huffman_codes.begin(), huffman_codes.end(), code, [](const Huffman_code hc, const std::uint32_t value) { return hc.code < value; })};

        if(iter != huffman_codes.end() and iter->code == code) {
            bitstream.skip_bits(i);
            return iter->symbol;
        }
    }

    throw Exception {Error::bad_formed_data};
}

void sel::impl::deflate::lz77_copy(std::vector<std::uint8_t>& inflated_data, const std::uint32_t length, const std::uint32_t distance)
{
    auto beginning_of_copy {inflated_data.end() - distance};
    auto copy_from_iter {beginning_of_copy};
    for(std::uint32_t i = 0u; i < length; ++i) {
        inflated_data.push_back(*copy_from_iter);
        ++copy_from_iter;
        if(copy_from_iter == inflated_data.end()) {
            copy_from_iter = beginning_of_copy;
        }
    }
}
