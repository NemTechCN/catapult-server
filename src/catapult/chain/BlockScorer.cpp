/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#include "BlockScorer.h"
#include "catapult/model/Block.h"
#include "catapult/model/ImportanceHeight.h"
#include "catapult/utils/IntegerMath.h"

namespace catapult { namespace chain {

	namespace {
		constexpr uint64_t Two_To_54 = 1ull << 54;

		struct GenerationHashInfo {
			uint32_t Value;
			uint32_t NumLeadingZeros;
		};

		constexpr utils::TimeSpan TimeBetweenBlocks(const model::Block& parent, const model::Block& block) {
			return utils::TimeSpan::FromDifference(block.Timestamp, parent.Timestamp);
		}

		uint32_t NumLeadingZeros(const Hash256& generationHash) {
			for (auto i = 0u; i < Hash256_Size; ++i) {
				if (0 != generationHash[i])
					return 8u * i + 7u - utils::Log2(generationHash[i]);
			}

			return 256u;
		}

#ifdef _MSC_VER
#define BSWAP(VAL) _byteswap_ulong(VAL)
#else
#define BSWAP(VAL) __builtin_bswap32(VAL)
#endif

		uint32_t ExtractFromHashAtPosition(const Hash256& hash, size_t index) {
			return BSWAP(*reinterpret_cast<const uint32_t*>(hash.data() + index));
		}

		GenerationHashInfo ExtractGenerationHashInfo(const Hash256& generationHash) {
			auto numLeadingZeros = NumLeadingZeros(generationHash);
			if (224 <= numLeadingZeros)
				return GenerationHashInfo{ ExtractFromHashAtPosition(generationHash, Hash256_Size - 4), 224 };

			auto quotient = numLeadingZeros / 8;
			auto remainder = numLeadingZeros % 8;
			auto value = ExtractFromHashAtPosition(generationHash, quotient);
			value <<= remainder;
			value += generationHash[quotient + 4] >> (8 - remainder);
			return GenerationHashInfo{ value, numLeadingZeros };
		}
	}

	uint64_t CalculateHit(const Hash256& generationHash) {
		// we want to calculate 2^54 * abs(log(x)), where x = value/2^256 and value is a 256 bit integer
		// note that x is always < 1, therefore log(x) is always negative
		// the original version used boost::multiprecision to convert the generation hash (interpreted as 256 bit integer) to a double
		// the new version uses only the 32 bits beginning at the first non-zero bit of the hash
		// this results in a slightly less exact calculation but the difference is less than one ppm
		auto hashInfo = ExtractGenerationHashInfo(generationHash);

		// handle edge cases
		if (0 == hashInfo.Value)
			return std::numeric_limits<uint64_t>::max();

		if (0xFFFFFFFF == hashInfo.Value)
			return 0;

		// calculate nearest integer for log2(value) * 2 ^ 54
		auto logValue = utils::Log2TimesPowerOfTwo(hashInfo.Value, 54);

		// result is 256 * 2^54 - logValue - (256 - 32 - hashInfo.NumLeadingZeros) * 2^54 which can be simplified
		boost::multiprecision::uint128_t result = (32 + hashInfo.NumLeadingZeros) * Two_To_54 - logValue;

		// divide by log2(e)
		result = result * 10'000'000'000'000'000ull / 14'426'950'408'889'634ull;
		return result.convert_to<uint64_t>();
	}

	uint64_t CalculateScore(const model::Block& parentBlock, const model::Block& currentBlock) {
		if (currentBlock.Timestamp <= parentBlock.Timestamp)
			return 0u;

		// r = difficulty(1) - (t(1) - t(0)) / MS_In_S
		auto timeDiff = TimeBetweenBlocks(parentBlock, currentBlock);
		return currentBlock.Difficulty.unwrap() - timeDiff.seconds();
	}

	namespace {
		BlockTarget GetMultiplier(uint64_t timeDiff, const model::BlockChainConfiguration& config) {
			auto targetTime = config.BlockGenerationTargetTime.seconds();
			double smoother = 1.0;
			if (0 != config.BlockTimeSmoothingFactor) {
				double factor = config.BlockTimeSmoothingFactor / 1000.0;
				smoother = std::min(std::exp(factor * static_cast<int64_t>(timeDiff - targetTime) / targetTime), 100.0);
			}

			BlockTarget target(static_cast<uint64_t>(Two_To_54 * smoother));
			target <<= 10;
			return target;
		}
	}

	BlockTarget CalculateTarget(
			const utils::TimeSpan& timeSpan,
			Difficulty difficulty,
			Importance signerImportance,
			const model::BlockChainConfiguration& config) {
		BlockTarget target = timeSpan.seconds();
		target *= signerImportance.unwrap();
		target *= GetMultiplier(timeSpan.seconds(), config);
		target *= 8'999'999'998; // scale to original total chain importance
		target /= config.TotalChainImportance.unwrap();
		target /= difficulty.unwrap();
		return target;
	}

	BlockTarget CalculateTarget(
			const model::Block& parentBlock,
			const model::Block& currentBlock,
			Importance signerImportance,
			const model::BlockChainConfiguration& config) {
		if (currentBlock.Timestamp <= parentBlock.Timestamp)
			return BlockTarget(0);

		auto timeDiff = TimeBetweenBlocks(parentBlock, currentBlock);
		return CalculateTarget(timeDiff, currentBlock.Difficulty, signerImportance, config);
	}

	BlockHitPredicate::BlockHitPredicate(const model::BlockChainConfiguration& config, const ImportanceLookupFunc& importanceLookup)
			: m_config(config)
			, m_importanceLookup(importanceLookup)
	{}

	bool BlockHitPredicate::operator()(const model::Block& parentBlock, const model::Block& block, const Hash256& generationHash) const {
		auto importance = m_importanceLookup(block.Signer, block.Height);
		auto hit = CalculateHit(generationHash);
		auto target = CalculateTarget(parentBlock, block, importance, m_config);
		return hit < target;
	}

	bool BlockHitPredicate::operator()(const BlockHitContext& context) const {
		auto importance = m_importanceLookup(context.Signer, context.Height);
		auto hit = CalculateHit(context.GenerationHash);
		auto target = CalculateTarget(context.ElapsedTime, context.Difficulty, importance, m_config);
		return hit < target;
	}
}}
