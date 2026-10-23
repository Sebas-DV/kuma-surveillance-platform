#include <gtest/gtest.h>

#include <set>

#include "security/Crypto.hpp"
#include "security/PasswordHasher.hpp"
#include "security/SecretBox.hpp"

namespace {

using namespace security;
using domain::ErrorKind;

constexpr std::string_view kKeyA = "MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY=";
constexpr std::string_view kKeyB = "ZmVkY2JhOTg3NjU0MzIxMGZlZGNiYTk4NzY1NDMyMTA=";

TEST(SecretBoxTest, RoundTripsPlaintext) {
    const auto box = SecretBox::fromBase64(kKeyA);
    ASSERT_TRUE(box);
    const auto sealed = box.value().seal("Xito1234");
    ASSERT_TRUE(sealed);
    const auto opened = box.value().open(sealed.value());
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened.value(), "Xito1234");
}

TEST(SecretBoxTest, CiphertextDoesNotContainPlaintextAndUsesFreshNonce) {
    const auto box = SecretBox::fromBase64(kKeyA);
    ASSERT_TRUE(box);
    const auto first = box.value().seal("super-secret-password");
    const auto second = box.value().seal("super-secret-password");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.value(), second.value());
    const std::string raw(first.value().begin(), first.value().end());
    EXPECT_EQ(raw.find("super-secret-password"), std::string::npos);
}

TEST(SecretBoxTest, WrongKeyFails) {
    const auto sealed = SecretBox::fromBase64(kKeyA).value().seal("password");
    ASSERT_TRUE(sealed);
    const auto opened = SecretBox::fromBase64(kKeyB).value().open(sealed.value());
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().kind, ErrorKind::InvalidArgument);
}

TEST(SecretBoxTest, TamperedCiphertextFails) {
    const auto box = SecretBox::fromBase64(kKeyA);
    auto sealed = box.value().seal("password");
    ASSERT_TRUE(sealed);
    auto bytes = sealed.value();
    bytes.back() ^= 0x01U;
    EXPECT_FALSE(box.value().open(bytes));
    bytes.resize(4);
    EXPECT_FALSE(box.value().open(bytes));
}

TEST(SecretBoxTest, EmptyPlaintextRoundTrips) {
    const auto box = SecretBox::fromBase64(kKeyA);
    const auto sealed = box.value().seal("");
    ASSERT_TRUE(sealed);
    const auto opened = box.value().open(sealed.value());
    ASSERT_TRUE(opened);
    EXPECT_TRUE(opened.value().empty());
}

TEST(SecretBoxTest, RejectsMalformedKeys) {
    EXPECT_FALSE(SecretBox::fromBase64(""));
    EXPECT_FALSE(SecretBox::fromBase64("c2hvcnQ="));
    EXPECT_FALSE(SecretBox::fromBase64("not base64 at all!!"));
    EXPECT_FALSE(SecretBox::fromBase64(std::string(kKeyA) + "AA"));
}

TEST(PasswordHasherTest, VerifiesCorrectPasswordOnly) {
    const auto hash = hashPassword("correct horse battery");
    ASSERT_TRUE(hash);
    EXPECT_TRUE(verifyPassword(hash.value(), "correct horse battery"));
    EXPECT_FALSE(verifyPassword(hash.value(), "correct horse batterY"));
    EXPECT_FALSE(verifyPassword(hash.value(), ""));
    EXPECT_NE(hash.value().find("$argon2id$"), std::string::npos);
}

TEST(PasswordHasherTest, SaltsEveryHash) {
    const auto first = hashPassword("correct horse battery");
    const auto second = hashPassword("correct horse battery");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.value(), second.value());
}

TEST(PasswordHasherTest, EnforcesLengthPolicy) {
    EXPECT_FALSE(hashPassword("short"));
    EXPECT_FALSE(hashPassword(std::string(kMaxPasswordLength + 1, 'a')));
    EXPECT_TRUE(hashPassword(std::string(kMinPasswordLength, 'a')));
}

TEST(PasswordHasherTest, RejectsGarbageHashes) {
    EXPECT_FALSE(verifyPassword("", "password-long-enough"));
    EXPECT_FALSE(verifyPassword("not-a-hash", "password-long-enough"));
    EXPECT_FALSE(verifyPassword(std::string(500, 'x'), "password-long-enough"));
    EXPECT_TRUE(passwordNeedsRehash("not-a-hash"));
}

TEST(CryptoTest, TokensAreUniquePrefixedAndUrlSafe) {
    std::set<std::string> seen;
    for (int index = 0; index < 200; ++index) {
        const auto token = generateToken("vtk_");
        ASSERT_TRUE(token);
        EXPECT_EQ(token.value().rfind("vtk_", 0), 0U);
        EXPECT_EQ(token.value().size(), 4U + 43U);
        EXPECT_EQ(token.value().find_first_not_of(
                      "vtk_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-"),
                  std::string::npos);
        EXPECT_TRUE(seen.insert(token.value()).second);
    }
}

TEST(CryptoTest, Sha256MatchesKnownVector) {
    EXPECT_EQ(toHex(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hashToken("abc"), sha256("abc"));
    EXPECT_EQ(sha256("").size(), kDigestSize);
}

TEST(CryptoTest, HexRoundTripsAndRejectsInvalidInput) {
    const Bytes original{0x00, 0x01, 0x7F, 0x80, 0xFF};
    const auto decoded = fromHex(toHex(original));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), original);
    EXPECT_TRUE(fromHex("ABcd"));
    EXPECT_FALSE(fromHex("abc"));
    EXPECT_FALSE(fromHex("zz"));
}

TEST(CryptoTest, WipeClearsString) {
    std::string secret = "secret";
    wipe(secret);
    EXPECT_TRUE(secret.empty());
}

}
