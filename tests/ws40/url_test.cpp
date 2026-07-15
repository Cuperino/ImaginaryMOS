// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// The MOS 4.0 connection-URL parameter scheme (plan §5.1): required
// parameters, passive mode, vendor-extra passthrough, hardening rejections.

#include <string>

#include <gtest/gtest.h>

#include <imos/ws40/url.h>

using imos::Channel;
using imos::ErrorCode;
using imos::ws40::composeQuery;
using imos::ws40::composeUrl;
using imos::ws40::ConnectionParams;
using imos::ws40::parseConnectionParams;
using imos::ws40::percentDecode;
using imos::ws40::percentEncode;

TEST(Ws40Url, ParsesSpecExampleUrl) {
    const auto params = parseConnectionParams(
        "wss://device.example.com/somepath/MyDeviceMos"
        "?param1=12345&mosID=MYMOSID&ncsID=MYNCSID&channel=mom");
    ASSERT_TRUE(params.ok());
    EXPECT_EQ(params.value().mosID, "MYMOSID");
    EXPECT_EQ(params.value().ncsID, "MYNCSID");
    EXPECT_EQ(params.value().channel, Channel::Mom);
    EXPECT_FALSE(params.value().passive);
    ASSERT_EQ(params.value().extras.size(), 1U);
    EXPECT_EQ(params.value().extras[0].first, "param1");
    EXPECT_EQ(params.value().extras[0].second, "12345");
}

TEST(Ws40Url, ParsesServerRequestUri) {
    const auto params = parseConnectionParams(
        "/mos/Communication?mosID=vtr7.playout.example.mos&ncsID=ncs.example.com"
        "&channel=ro&passive=true");
    ASSERT_TRUE(params.ok());
    EXPECT_EQ(params.value().channel, Channel::Ro);
    EXPECT_TRUE(params.value().passive);
    EXPECT_TRUE(params.value().extras.empty());
}

TEST(Ws40Url, PercentDecodesMosParameters) {
    const auto params =
        parseConnectionParams("/mos?mosID=vtr%207.example.mos&ncsID=ncs%2Eexample.com&channel=aux");
    ASSERT_TRUE(params.ok());
    EXPECT_EQ(params.value().mosID, "vtr 7.example.mos");
    EXPECT_EQ(params.value().ncsID, "ncs.example.com");
    EXPECT_EQ(params.value().channel, Channel::Aux);
}

TEST(Ws40Url, MissingParametersAreNamed) {
    const auto params = parseConnectionParams("/mos?mosID=a");
    ASSERT_FALSE(params.ok());
    EXPECT_EQ(params.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(params.error().message.find("ncsID"), std::string::npos);
    EXPECT_NE(params.error().message.find("channel"), std::string::npos);
    EXPECT_EQ(params.error().message.find("mosID"), std::string::npos);
}

TEST(Ws40Url, DuplicateMosParameterIsRejected) {
    const auto params = parseConnectionParams("/mos?mosID=a&ncsID=b&channel=mom&mosID=evil");
    ASSERT_FALSE(params.ok());
    EXPECT_NE(params.error().message.find("duplicate mosID"), std::string::npos);
}

TEST(Ws40Url, BadChannelAndPassiveValuesAreRejected) {
    auto params = parseConnectionParams("/mos?mosID=a&ncsID=b&channel=lower");
    ASSERT_FALSE(params.ok());
    EXPECT_NE(params.error().message.find("mom, ro, or aux"), std::string::npos);

    params = parseConnectionParams("/mos?mosID=a&ncsID=b&channel=mom&passive=false");
    ASSERT_FALSE(params.ok());
    EXPECT_NE(params.error().message.find("passive"), std::string::npos);

    // The spec spells it "true"; case-insensitive acceptance is Postel room.
    params = parseConnectionParams("/mos?mosID=a&ncsID=b&channel=mom&passive=True");
    ASSERT_TRUE(params.ok());
    EXPECT_TRUE(params.value().passive);
}

TEST(Ws40Url, MalformedPercentEncodingIsRejected) {
    EXPECT_FALSE(parseConnectionParams("/mos?mosID=a%2&ncsID=b&channel=mom").ok());
    EXPECT_FALSE(parseConnectionParams("/mos?mosID=a%GG&ncsID=b&channel=mom").ok());
    EXPECT_FALSE(parseConnectionParams("/mos?mosID=a%00b&ncsID=b&channel=mom").ok());
}

TEST(Ws40Url, OversizedUriIsRejectedBeforeParsing) {
    std::string uri = "/mos?mosID=a&ncsID=b&channel=mom&pad=";
    uri += std::string(imos::ws40::kMaxUriBytes, 'x');
    const auto params = parseConnectionParams(uri);
    ASSERT_FALSE(params.ok());
    EXPECT_EQ(params.error().code, ErrorCode::LimitExceeded);
}

TEST(Ws40Url, VendorExtrasSurviveRoundTripVerbatim) {
    const std::string uri = "/mos?apiKey=Zm9v%2Fbar&flag&mosID=a&ncsID=b&channel=mom&vendor=x%20y";
    const auto params = parseConnectionParams(uri);
    ASSERT_TRUE(params.ok());
    ASSERT_EQ(params.value().extras.size(), 3U);
    EXPECT_EQ(params.value().extras[0].first, "apiKey");
    EXPECT_EQ(params.value().extras[0].second, "Zm9v%2Fbar");  // wire form, undecoded
    EXPECT_EQ(params.value().extras[1].first, "flag");
    EXPECT_EQ(params.value().extras[1].second, "");
    EXPECT_EQ(params.value().extras[2].second, "x%20y");

    const std::string query = composeQuery(params.value());
    EXPECT_EQ(query, "apiKey=Zm9v%2Fbar&flag&vendor=x%20y&mosID=a&ncsID=b&channel=mom");
}

TEST(Ws40Url, ComposeParseRoundTrip) {
    ConnectionParams params;
    params.mosID = "vtr 7/. example.mos";  // needs encoding
    params.ncsID = "ncs.example.com";
    params.channel = Channel::Aux;
    params.passive = true;
    const std::string url = composeUrl("wss://host/mos/Communication", params);
    EXPECT_EQ(url.find('?'), std::string{"wss://host/mos/Communication"}.size());

    const auto reparsed = parseConnectionParams(url);
    ASSERT_TRUE(reparsed.ok());
    EXPECT_EQ(reparsed.value().mosID, params.mosID);
    EXPECT_EQ(reparsed.value().ncsID, params.ncsID);
    EXPECT_EQ(reparsed.value().channel, Channel::Aux);
    EXPECT_TRUE(reparsed.value().passive);
}

TEST(Ws40Url, ComposeUrlAppendsToExistingQuery) {
    ConnectionParams params;
    params.mosID = "a";
    params.ncsID = "b";
    params.channel = Channel::Mom;
    const std::string url = composeUrl("wss://host/mos?vendor=1", params);
    EXPECT_EQ(url, "wss://host/mos?vendor=1&mosID=a&ncsID=b&channel=mom");
}

TEST(Ws40Url, PercentEncodeDecode) {
    EXPECT_EQ(percentEncode("a-b_c.d~e"), "a-b_c.d~e");
    EXPECT_EQ(percentEncode("a b&c=d%"), "a%20b%26c%3Dd%25");
    const auto decoded = percentDecode("a%20b%26c%3Dd%25");
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), "a b&c=d%");
}
