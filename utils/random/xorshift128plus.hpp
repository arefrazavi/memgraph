#pragma once

#include <cstdlib>
#include <random>

/* Xorshift algorithm (plus variant)
 *
 * This is the fastest generator passing BigCrush without systematic failures,
 * but due to the relatively short period it is acceptable only for
 * applications with a mild amount of parallelism, otherwise, use a
 * xorshift1024* generator.
 */
struct Xorshift128plus
{
public:
    Xorshift128plus()
    {
        // use a slow, more complex rnd generator to initialize a fast one
        // make sure to call this before requesting any random numbers!
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;

        // the number generated by MT can be full of zeros and xorshift
        // doesn't like this so we use MurmurHash3 64bit finalizer to
        // make it less biased
        s[0] = avalance(dist(gen));
        s[1] = avalance(dist(gen));
    }

    uint64_t operator()()
    {
        uint64_t s1 = s[0];
        const uint64_t s0 = s[1];

        s[0] = s0;
        s1 ^= s1 << 23;

        return (s[1] = (s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26))) + s0;
    }

private:
    uint64_t s[2];

    uint64_t avalance(uint64_t s)
    {
        // MurmurHash3 finalizer
        s ^= s >> 33;
        s *= 0xff51afd7ed558ccd;
        s ^= s >> 33;
        s *= 0xc4ceb9fe1a85ec53;
        s ^= s >> 33;

        return s;
    }
};
