// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/private/batch.h"

#include "base64/base64.h"

#include <gtest/gtest.h>

using json = nlohmann::json;
using Webserver::BatchTimeoutMs;
using Webserver::MaxBatchOps;
using Webserver::MaxBatchReadBytes;
using Webserver::MaxBatchWriteBytes;
using Webserver::ParseBatchOps;
using Webserver::ParseOnError;

namespace {

// ---------------------------------------------------------------------
// BatchTimeoutMs - pure arithmetic, no emulator state needed
// ---------------------------------------------------------------------

TEST(BatchTimeoutMs, ScalesLinearlyWithOpCount)
{
	EXPECT_EQ(BatchTimeoutMs(0), 250u);
	EXPECT_EQ(BatchTimeoutMs(1), 254u);
	EXPECT_EQ(BatchTimeoutMs(10), 290u);
}

TEST(BatchTimeoutMs, CapsAtTwoThousandMs)
{
	// 250 + 4*64 = 506, well under the cap - confirms MaxBatchOps
	// itself never reaches the cap, only a hypothetically larger count
	// would.
	EXPECT_EQ(BatchTimeoutMs(MaxBatchOps), 250u + 4u * MaxBatchOps);
	EXPECT_EQ(BatchTimeoutMs(1000), 2000u);
	EXPECT_EQ(BatchTimeoutMs(1'000'000), 2000u);
}

// ---------------------------------------------------------------------
// ParseOnError
// ---------------------------------------------------------------------

TEST(ParseOnError, DefaultsToAbortWhenAbsent)
{
	EXPECT_TRUE(ParseOnError(json::object()));
}

TEST(ParseOnError, ParsesAbortAndContinue)
{
	EXPECT_TRUE(ParseOnError(json{
	        {"on_error", "abort"}
        }));
	EXPECT_FALSE(ParseOnError(json{
	        {"on_error", "continue"}
        }));
}

TEST(ParseOnError, RejectsAnUnknownValue)
{
	EXPECT_THROW(ParseOnError(json{
	                     {"on_error", "retry"}
        }),
	             std::invalid_argument);
}

// ---------------------------------------------------------------------
// ParseBatchOps - structural validation. Every case here deliberately
// avoids freeze_set: ValidateFreezeRange (called from within
// ParseBatchOps) needs MEM_TotalPages()*MemPageSize, which is 0 in this
// unit test binary (no emulator core is initialised here, matching
// every other MEM_TotalPages()-dependent route in this codebase - see
// 1.4's own note that such code has no ctest coverage and is verified
// against the real binary instead). freeze_set's *own* structural
// checks (width in {1,2,4}) still run before that call and are covered
// below; freeze_clear never calls it at all.
// ---------------------------------------------------------------------

TEST(ParseBatchOps, RejectsAMissingOpsField)
{
	EXPECT_THROW(ParseBatchOps(json::object()), std::invalid_argument);
}

TEST(ParseBatchOps, RejectsAnEmptyOpsArray)
{
	EXPECT_THROW(ParseBatchOps(json{
	                     {"ops", json::array()}
        }),
	             std::invalid_argument);
}

TEST(ParseBatchOps, RejectsMoreThanMaxBatchOps)
{
	json ops = json::array();
	for (size_t i = 0; i < MaxBatchOps + 1; i++) {
		ops.push_back({
		        {"op", "cpu_read"}
                });
	}
	EXPECT_THROW(ParseBatchOps(json{
	                     {"ops", ops}
        }),
	             std::invalid_argument);
}

TEST(ParseBatchOps, AcceptsExactlyMaxBatchOps)
{
	json ops = json::array();
	for (size_t i = 0; i < MaxBatchOps; i++) {
		ops.push_back({
		        {"op", "cpu_read"}
                });
	}
	const auto parsed = ParseBatchOps(json{
	        {"ops", ops}
        });
	EXPECT_EQ(parsed.size(), MaxBatchOps);
}

TEST(ParseBatchOps, RejectsAnUnknownOpType)
{
	EXPECT_THROW(ParseBatchOps(json{
	                     {"ops", json::array({{{"op", "debug_pause"}}})}
        }),
	             std::invalid_argument);
}

TEST(ParseBatchOps, RejectsAnOpWithNoOpField)
{
	EXPECT_THROW(ParseBatchOps(json{
	                     {"ops", json::array({json::object()})}
        }),
	             std::invalid_argument);
}

TEST(ParseBatchOps, ParsesCpuRead)
{
	const auto ops = ParseBatchOps(json{
	        {"ops", json::array({{{"op", "cpu_read"}}})}
        });
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].type, Webserver::BatchOpType::CpuRead);
}

TEST(ParseBatchOps, CpuWriteRejectsAnUnknownRegister)
{
	json body{
	        {"ops",
	         json::array({{{"op", "cpu_write"}, {"register", "xax"}, {"value", 1}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, CpuWriteRejectsAnOversizedSegmentValue)
{
	json body{
	        {"ops",
	         json::array({{{"op", "cpu_write"},
	                       {"register", "ds"},
	                       {"value", 0x10000}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, CpuWriteAcceptsAValidGeneralRegister)
{
	json body{
	        {"ops",
	         json::array({{{"op", "cpu_write"},
	                       {"register", "eax"},
	                       {"value", 1234}}})}
        };
	const auto ops = ParseBatchOps(body);
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].cpu_value, 1234u);
}

TEST(ParseBatchOps, MemReadRejectsAZeroLength)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"}, {"offset", 0}, {"len", 0}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemReadAggregatesLengthAgainstTheReadCap)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"}, {"offset", 0}, {"len", MaxBatchReadBytes}},
	                      {{"op", "mem_read"}, {"offset", 0}, {"len", 1}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemReadAcceptsExactlyTheReadCapInOneOp)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"},
	                       {"offset", 0},
	                       {"len", MaxBatchReadBytes}}})}
        };
	const auto ops = ParseBatchOps(body);
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].length, MaxBatchReadBytes);
}

TEST(ParseBatchOps, MemWriteRejectsAnExpectedField)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_write"},
	                       {"offset", 0},
	                       {"data", "AA=="},
	                       {"expected", "AA=="}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemCasRequiresAnExpectedField)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_cas"}, {"offset", 0}, {"data", "AA=="}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemCasAcceptsDataAndExpected)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_cas"},
	                       {"offset", 0},
	                       {"data", "d29ybGQ="},
	                       {"expected", "aGVsbG8="}}})}
        };
	const auto ops = ParseBatchOps(body);
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].type, Webserver::BatchOpType::MemCas);
	EXPECT_EQ(ops[0].data, "world");
	EXPECT_EQ(ops[0].expected, "hello");
}

TEST(ParseBatchOps, MemReadRejectsLenAbovePerOpMaxMemoryTransferBytes)
{
	// Distinct from MemReadAggregatesLengthAgainstTheReadCap: this
	// exceeds the per-op MaxMemoryTransferBytes (128 MiB) ceiling in a
	// single op, well above the much tighter aggregate MaxBatchReadBytes
	// (1 MiB) - the per-op guard must fire with its own message, not the
	// aggregate one.
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"},
	                       {"offset", 0},
	                       {"len", Webserver::MaxMemoryTransferBytes + 1}}})}
        };
	try {
		ParseBatchOps(body);
		FAIL() << "expected std::invalid_argument";
	} catch (const std::invalid_argument& e) {
		EXPECT_NE(std::string(e.what()).find("len must be 1.."),
		          std::string::npos);
	}
}

TEST(ParseBatchOps, MemAddrRejectsANumericSegmentOffsetOverflow)
{
	// segment=0xFFFF -> paragraph address 0xFFFF0; adding an offset
	// this close to UINT32_MAX overflows uint32_t if computed in 32-bit
	// arithmetic, wrapping to a small, in-range-looking address instead
	// of correctly failing - mirrors
	// FreezeRangeValidation.RejectsAddressNearUint32MaxWithout32BitWraparound
	// (webserver_freeze_tests.cpp), which treats this exact class of bug
	// as worth its own dedicated test.
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"},
	                       {"segment", 0xFFFF},
	                       {"offset", 0xFFFFFF00},
	                       {"len", 1}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemWriteAggregatesDataAgainstTheWriteCap)
{
	// Each op's own decoded size is well under MaxBatchWriteBytes; only
	// their sum exceeds it, so this must be the aggregate check
	// catching it, not either op's individual size.
	const auto half_b64 = base64::to_base64(
	        std::string(MaxBatchWriteBytes / 2 + 1, 'A'));
	json body{
	        {"ops",
	         json::array(
	                 {{{"op", "mem_write"}, {"offset", 0}, {"data", half_b64}},
	                  {{"op", "mem_write"}, {"offset", 0}, {"data", half_b64}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemAddrRejectsANonRegisterSegmentString)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"},
	                       {"segment", "1234"},
	                       {"offset", 0},
	                       {"len", 1}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemAddrRejectsAnOversizedNumericSegment)
{
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"},
	                       {"segment", 0x10000},
	                       {"offset", 0},
	                       {"len", 1}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, MemAddrResolvesANumericSegmentIntoOffset)
{
	// 0x1234:0 -> 0x1234 * 16 = 0x12340
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"},
	                       {"segment", 0x1234},
	                       {"offset", 0},
	                       {"len", 1}}})}
        };
	const auto ops = ParseBatchOps(body);
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].segment, Webserver::Segment::None);
	EXPECT_EQ(ops[0].offset, 0x12340u);
}

TEST(ParseBatchOps, MemAddrAcceptsARegisterNameSegmentUnresolved)
{
	// A register segment resolves live at Execute() time - the parser
	// only records which register, leaving offset as given.
	json body{
	        {"ops",
	         json::array({{{"op", "mem_read"},
	                       {"segment", "ds"},
	                       {"offset", 100},
	                       {"len", 1}}})}
        };
	const auto ops = ParseBatchOps(body);
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].segment, Webserver::Segment::DS);
	EXPECT_EQ(ops[0].offset, 100u);
}

TEST(ParseBatchOps, PortReadRejectsAnOutOfRangePort)
{
	json body{
	        {"ops", json::array({{{"op", "port_read"}, {"port", 0x10000}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, PortWriteRejectsAnInvalidWidth)
{
	json body{
	        {"ops",
	         json::array({{{"op", "port_write"},
	                       {"port", 0x60},
	                       {"width", 4},
	                       {"value", 1}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, PortReadDefaultsWidthToOne)
{
	json body{
	        {"ops", json::array({{{"op", "port_read"}, {"port", 0x60}}})}
        };
	const auto ops = ParseBatchOps(body);
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].port_width, 1);
}

TEST(ParseBatchOps, FreezeSetRejectsAnInvalidWidthBeforeTheRangeCheck)
{
	// width validation runs before ValidateFreezeRange - reachable in
	// this unit test binary even with MEM_TotalPages()==0.
	json body{
	        {"ops",
	         json::array({{{"op", "freeze_set"},
	                       {"address", 0},
	                       {"value", 1},
	                       {"width", 3}}})}
        };
	EXPECT_THROW(ParseBatchOps(body), std::invalid_argument);
}

TEST(ParseBatchOps, FreezeClearParsesWithoutTouchingMemTotal)
{
	// freeze_clear never calls ValidateFreezeRange, so - unlike
	// freeze_set - this succeeds even with no live emulator memory.
	json body{
	        {"ops", json::array({{{"op", "freeze_clear"}, {"address", 12345}}})}
        };
	const auto ops = ParseBatchOps(body);
	ASSERT_EQ(ops.size(), 1u);
	EXPECT_EQ(ops[0].freeze_address, 12345u);
}

TEST(ParseBatchOps, ErrorMessageNamesTheFailingOpIndex)
{
	json body{
	        {"ops", json::array({{{"op", "cpu_read"}}, {{"op", "bogus"}}})}
        };
	try {
		ParseBatchOps(body);
		FAIL() << "expected std::invalid_argument";
	} catch (const std::invalid_argument& e) {
		EXPECT_NE(std::string(e.what()).find("ops[1"), std::string::npos);
	}
}

} // namespace
