// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "webserver/input.h"

#include <gtest/gtest.h>
#include <string>

#include "json/json.h"

using Webserver::InputSequenceCommand;
using Webserver::InputTypeCommand;
using Webserver::MaxEventFrame;
using Webserver::MaxEventTimeMs;
using Webserver::MaxInputEvents;
using Webserver::MaxTypedTextChars;
using Webserver::MaxTypingCps;
using Webserver::MinTypingCps;
using Webserver::RecordingStoreHandlers;

namespace RecordingStore = Webserver::RecordingStore;

namespace {

// -- InputSequenceCommand::Post: HTTP-level request handling --
//
// Every branch below responds directly (sets res.status, sends the
// body, returns) before InputSequenceCommand is constructed or the
// Bridge is touched - this is input.cpp's largest block of untrusted-
// input parsing (~270 lines), and none of it needs a booted emulator
// or a live Bridge pump to exercise. The one exception is the
// {"recording": "<name>"} form resolving a *known* name: RecordingStore
// itself is a plain web-thread-only map (no Bridge), so a 404 for an
// unknown name is covered here too; actually replaying either form
// still needs the Bridge and isn't attempted.

TEST(InputSequencePost, RejectsBothEventsAndRecording)
{
	httplib::Request req;
	req.body = R"({"events": [], "recording": "x"})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("either"), std::string::npos);
}

TEST(InputSequencePost, RejectsNonStringRecording)
{
	httplib::Request req;
	req.body = R"({"recording": 123})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("error").get<std::string>(), "'recording' must be a string");
}

TEST(InputSequencePost, RejectsInvalidRecordingName)
{
	httplib::Request req;
	req.body = R"({"recording": "not a valid name!"})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Invalid recording name"),
	          std::string::npos);
}

TEST(InputSequencePost, UnknownRecordingNameIs404)
{
	// RecordingStore::Get runs before any Command is constructed, so a
	// genuinely unknown (but validly-shaped) name is safe to exercise
	// directly - unlike a known name, which would go on to replay
	// through the Bridge.
	httplib::Request req;
	req.body = R"({"recording": "definitely-does-not-exist-12345"})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 404);
}

TEST(InputSequencePost, RejectsMissingEventsField)
{
	httplib::Request req;
	req.body = "{}";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("events"), std::string::npos);
}

TEST(InputSequencePost, RejectsEventsNotAnArray)
{
	httplib::Request req;
	req.body = R"({"events": "nope"})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
}

TEST(InputSequencePost, RejectsTooManyEvents)
{
	std::string body = R"({"events": [)";
	for (size_t i = 0; i < MaxInputEvents + 1; ++i) {
		if (i) {
			body += ",";
		}
		body += R"({"type": "key", "key": "KBD_a"})";
	}
	body += "]}";

	httplib::Request req;
	req.body = body;
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Too many events"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsUnknownFieldForEventType)
{
	httplib::Request req;
	req.body = R"({"events": [{"type": "key", "key": "KBD_a", "x_rel": 1}]})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Unknown field"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsBothTAndDelayMs)
{
	httplib::Request req;
	req.body = R"({"events": [{"type": "key", "key": "KBD_a", "t": 1, "delay_ms": 1}]})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("not both"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsOutOfRangeT)
{
	httplib::Request req;
	req.body = "{\"events\": [{\"type\": \"key\", \"key\": \"KBD_a\", \"t\": " +
	           std::to_string(MaxEventTimeMs + 1) + "}]}";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("'t'"), std::string::npos);
}

TEST(InputSequencePost, RejectsOutOfRangeDelayMs)
{
	httplib::Request req;
	req.body =
	        "{\"events\": [{\"type\": \"key\", \"key\": \"KBD_a\", "
	        "\"delay_ms\": " +
	        std::to_string(MaxEventTimeMs + 1) + "}]}";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("'delay_ms'"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsAccumulatedDelayMsPastTheLimit)
{
	// Two individually-valid delays that sum past MaxEventTimeMs - the
	// cumulative check, not the per-event one.
	const double half_plus = MaxEventTimeMs / 2 + 1;
	httplib::Request req;
	req.body =
	        "{\"events\": ["
	        "{\"type\": \"key\", \"key\": \"KBD_a\", \"delay_ms\": " +
	        std::to_string(half_plus) +
	        "},"
	        "{\"type\": \"key\", \"key\": \"KBD_b\", \"delay_ms\": " +
	        std::to_string(half_plus) + "}]}";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Accumulated"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsOutOfRangeFrame)
{
	httplib::Request req;
	req.body = "{\"events\": [{\"type\": \"key\", \"key\": \"KBD_a\", \"frame\": " +
	           std::to_string(MaxEventFrame + 1) + "}]}";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("'frame'"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsUnknownKeyName)
{
	httplib::Request req;
	req.body = R"({"events": [{"type": "key", "key": "KBD_NOT_A_REAL_KEY"}]})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Unknown key"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsMouseMoveWithOnlyXAbs)
{
	httplib::Request req;
	req.body = R"({"events": [{"type": "mouse_move", "x_abs": 100}]})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("together"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsMouseMoveWithOutOfRangeXAbs)
{
	httplib::Request req;
	req.body = R"({"events": [{"type": "mouse_move", "x_abs": -1, "y_abs": 0}]})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
}

TEST(InputSequencePost, RejectsUnknownButton)
{
	httplib::Request req;
	req.body = R"({"events": [{"type": "mouse_button", "button": "nope"}]})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Unknown button"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsUnknownEventType)
{
	httplib::Request req;
	req.body = R"({"events": [{"type": "not_a_real_type"}]})";
	httplib::Response res;
	InputSequenceCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Unknown event type"),
	          std::string::npos);
}

TEST(InputSequencePost, RejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(InputSequenceCommand::Post(req, res), std::exception);
}

// -- InputTypeCommand::Post: HTTP-level request handling --

TEST(InputTypePost, RejectsMissingText)
{
	httplib::Request req;
	req.body = "{}";
	httplib::Response res;
	InputTypeCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("'text'"), std::string::npos);
}

TEST(InputTypePost, RejectsTextOverTheLengthCap)
{
	httplib::Request req;
	req.body = R"({"text": ")" + std::string(MaxTypedTextChars + 1, 'a') + "\"}";
	httplib::Response res;
	InputTypeCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("Text too long"),
	          std::string::npos);
}

TEST(InputTypePost, RejectsOutOfRangeCps)
{
	httplib::Request req;
	req.body = "{\"text\": \"hi\", \"cps\": " +
	           std::to_string(MaxTypingCps + 1) + "}";
	httplib::Response res;
	InputTypeCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_NE(j.at("error").get<std::string>().find("'cps'"), std::string::npos);
}

TEST(InputTypePost, RejectsCpsBelowMinimum)
{
	httplib::Request req;
	req.body = "{\"text\": \"hi\", \"cps\": " +
	           std::to_string(MinTypingCps / 2) + "}";
	httplib::Response res;
	InputTypeCommand::Post(req, res);

	EXPECT_EQ(res.status, 400);
}

TEST(InputTypePost, RejectsMalformedJson)
{
	httplib::Request req;
	req.body = "not json";
	httplib::Response res;
	EXPECT_THROW(InputTypeCommand::Post(req, res), std::exception);
}

// -- RecordingStoreHandlers::GetList / Delete: never Command-backed at
// all, RecordingStore is a plain web-thread-only map. RecordingStore is
// a process-wide static, shared with every other test in this binary
// (RecordingStoreTest in webserver_input_validation_tests.cpp already
// notes this) - each test here uses a name unlikely to collide and
// deletes it afterward.

TEST(RecordingStoreHandlersTest, DeleteOfUnknownNameIs404)
{
	httplib::Request req;
	req.path_params["name"] = "definitely-does-not-exist-67890";
	httplib::Response res;
	RecordingStoreHandlers::Delete(req, res);

	EXPECT_EQ(res.status, 404);
}

TEST(RecordingStoreHandlersTest, DeleteOfKnownNameSucceedsAndRemovesIt)
{
	const std::string name = "webserver-input-sequence-tests-delete-me";
	RecordingStore::Save(name, {}, false, 0.0);

	httplib::Request req;
	req.path_params["name"] = name;
	httplib::Response res;
	RecordingStoreHandlers::Delete(req, res);

	EXPECT_NE(res.status, 404);
	const auto j = nlohmann::json::parse(res.body);
	EXPECT_EQ(j.at("status").get<std::string>(), "deleted");
	EXPECT_EQ(j.at("name").get<std::string>(), name);
	EXPECT_FALSE(RecordingStore::Get(name).has_value());
}

TEST(RecordingStoreHandlersTest, GetListIncludesASeededEntry)
{
	const std::string name = "webserver-input-sequence-tests-list-me";
	RecordingStore::Save(name, {}, true, 42.0);

	httplib::Request req;
	httplib::Response res;
	RecordingStoreHandlers::GetList(req, res);

	const auto j = nlohmann::json::parse(res.body);
	bool found   = false;
	for (const auto& entry : j.at("recordings")) {
		if (entry.at("name").get<std::string>() == name) {
			found = true;
			EXPECT_EQ(entry.at("event_count").get<int>(), 0);
			EXPECT_EQ(entry.at("duration_ms").get<double>(), 42.0);
			EXPECT_TRUE(entry.at("truncated").get<bool>());
		}
	}
	EXPECT_TRUE(found);

	RecordingStore::Delete(name);
}

} // namespace
