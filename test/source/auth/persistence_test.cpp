#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ctime>

#include "auth/auth.pb.h"

class PersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override { test_file_ = "auth_persistence_test.bin"; }

  void TearDown() override {
    if (std::filesystem::exists(test_file_)) {
      std::filesystem::remove(test_file_);
    }
  }

  std::string test_file_;
};

TEST_F(PersistenceTest, InvalidFileFormat) {
  std::ofstream file(test_file_, std::ios::binary);
  ASSERT_TRUE(file.is_open());
  file << "not a proto";
  file.close();

  std::ifstream input(test_file_, std::ios::binary);
  ASSERT_TRUE(input.is_open());
  std::string data((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  input.close();

  auth::v1::LicenseResp message;
  EXPECT_FALSE(message.ParseFromString(data));
}

TEST_F(PersistenceTest, ValidSerializationRoundTrip) {
  auth::v1::LicenseResp original;
  original.set_request_id("request-42");
  auto* asset = original.add_assets();
  asset->set_sn("SN0001");
  asset->set_name("Primary Asset");
  asset->set_created_at(std::time(nullptr) - 120);
  asset->set_is_expired(false);
  asset->set_expire_at(std::time(nullptr) + 7200);
  asset->set_remaining_valid_timestamp(7200);

  std::string serialized;
  ASSERT_TRUE(original.SerializeToString(&serialized));

  std::ofstream out(test_file_, std::ios::binary);
  ASSERT_TRUE(out.is_open());
  out << serialized;
  out.close();

  std::ifstream in(test_file_, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  std::string read_data((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  in.close();

  auth::v1::LicenseResp parsed;
  ASSERT_TRUE(parsed.ParseFromString(read_data));

  EXPECT_EQ(parsed.request_id(), original.request_id());
  ASSERT_EQ(parsed.assets_size(), 1);
  const auto& parsed_asset = parsed.assets(0);
  EXPECT_EQ(parsed_asset.sn(), "SN0001");
  EXPECT_EQ(parsed_asset.name(), "Primary Asset");
  EXPECT_FALSE(parsed_asset.is_expired());
  EXPECT_GT(parsed_asset.expire_at(), std::time(nullptr));
  EXPECT_GT(parsed_asset.remaining_valid_timestamp(), 0);
}
