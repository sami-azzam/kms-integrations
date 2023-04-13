// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kmscng/main/bridge.h"

#include "absl/cleanup/cleanup.h"
#include "common/kms_v1.h"
#include "common/test/resource_helpers.h"
#include "common/test/test_status_macros.h"
#include "gmock/gmock.h"
#include "kmscng/cng_headers.h"
#include "kmscng/object.h"
#include "kmscng/operation/sign_utils.h"
#include "kmscng/provider.h"
#include "kmscng/test/matchers.h"
#include "kmscng/util/string_utils.h"
#include "kmsp11/util/crypto_utils.h"

namespace cloud_kms::kmscng {
namespace {

// TODO(b/270419822): drop these once crypto_utils has been migrated to common.
using cloud_kms::kmsp11::EcdsaVerifyP1363;
using cloud_kms::kmsp11::ParseX509PublicKeyDer;

// TODO(b/277099517): replace default arguments with options struct.
kms_v1::CryptoKeyVersion NewCryptoKeyVersion(
    kms_v1::KeyManagementService::Stub* client,
    kms_v1::CryptoKey::CryptoKeyPurpose purpose =
        kms_v1::CryptoKey::ASYMMETRIC_SIGN,
    kms_v1::CryptoKeyVersion::CryptoKeyVersionAlgorithm algorithm =
        kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256,
    kms_v1::ProtectionLevel protection_level = kms_v1::ProtectionLevel::HSM) {
  kms_v1::KeyRing kr1;
  kr1 = CreateKeyRingOrDie(client, kTestLocation, RandomId(), kr1);

  kms_v1::CryptoKey ck;
  ck.set_purpose(purpose);
  ck.mutable_version_template()->set_algorithm(algorithm);
  ck.mutable_version_template()->set_protection_level(protection_level);
  ck = CreateCryptoKeyOrDie(client, kr1.name(), "ck", ck, true);

  kms_v1::CryptoKeyVersion ckv;
  ckv = CreateCryptoKeyVersionOrDie(client, ck.name(), ckv);
  ckv = WaitForEnablement(client, ckv);
  return ckv;
}

void SetFakeKmsProviderProperties(Provider* provider, std::string listen_addr) {
  // Set custom properties to hit fake KMS.
  EXPECT_OK(provider->SetProperty(kEndpointAddressProperty, listen_addr));
  EXPECT_OK(provider->SetProperty(kChannelCredentialsProperty, "insecure"));
}

TEST(BridgeTest, OpenProviderSuccess) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, OpenProviderInvalidHandle) {
  EXPECT_THAT(OpenProvider(nullptr, kProviderName.data(), 0),
              StatusSsIs(NTE_INVALID_PARAMETER));
}

TEST(BridgeTest, OpenProviderUnexpectedName) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_THAT(OpenProvider(&provider_handle, MS_KEY_STORAGE_PROVIDER, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));
}

TEST(BridgeTest, OpenProviderInvalidFlag) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_THAT(OpenProvider(&provider_handle, kProviderName.data(),
                           NCRYPT_PERSIST_ONLY_FLAG),
              StatusSsIs(NTE_BAD_FLAGS));
}

TEST(BridgeTest, FreeProviderSuccess) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetProviderPropertyGetSizeSuccess) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD output_size = 0;
  EXPECT_OK(GetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY,
                                nullptr, sizeof(DWORD), &output_size, 0));
  EXPECT_EQ(output_size, sizeof(DWORD));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetProviderPropertySuccess) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD output = 0;
  DWORD output_size = 0;
  EXPECT_OK(GetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY,
                                reinterpret_cast<uint8_t*>(&output),
                                sizeof(output), &output_size, 0));
  EXPECT_EQ(output_size, sizeof(output));
  EXPECT_EQ(output, NCRYPT_IMPL_HARDWARE_FLAG);

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetProviderPropertyInvalidHandle) {
  DWORD output_size;
  EXPECT_THAT(GetProviderProperty(0, NCRYPT_IMPL_TYPE_PROPERTY, nullptr,
                                  sizeof(DWORD), &output_size, 0),
              StatusSsIs(NTE_INVALID_HANDLE));
}

TEST(BridgeTest, GetProviderPropertyNameNull) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD output_size;
  EXPECT_THAT(GetProviderProperty(provider_handle, nullptr, nullptr,
                                  sizeof(DWORD), &output_size, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetProviderPropertyInvalidName) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD output_size;
  EXPECT_THAT(GetProviderProperty(provider_handle, NCRYPT_UI_POLICY_PROPERTY,
                                  nullptr, sizeof(DWORD), &output_size, 0),
              StatusSsIs(NTE_NOT_SUPPORTED));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetProviderPropertyOutputSizeBufferNull) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(GetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY,
                                  nullptr, sizeof(DWORD), nullptr, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetProviderPropertyOutputBufferTooShort) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  uint8_t output;
  DWORD output_size;
  EXPECT_THAT(GetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY,
                                  &output, 1, &output_size, 0),
              StatusSsIs(NTE_BUFFER_TOO_SMALL));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetProviderPropertyInvalidFlag) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD output_size = 0;
  EXPECT_THAT(GetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY,
                                  nullptr, sizeof(DWORD), &output_size,
                                  NCRYPT_PERSIST_ONLY_FLAG),
              StatusSsIs(NTE_BAD_FLAGS));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, SetProviderPropertySuccess) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  std::string input = "insecure";
  EXPECT_OK(SetProviderProperty(
      provider_handle, kChannelCredentialsProperty.data(),
      reinterpret_cast<uint8_t*>(input.data()), input.size(), 0));

  // Check that the provider property has been updated.
  std::string output("0", input.size());
  DWORD output_size = 0;
  EXPECT_OK(GetProviderProperty(provider_handle,
                                kChannelCredentialsProperty.data(),
                                reinterpret_cast<uint8_t*>(output.data()),
                                input.size(), &output_size, 0));
  EXPECT_EQ(output_size, output.size());
  EXPECT_EQ(output, "insecure");

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, SetProviderPropertyInvalidHandle) {
  EXPECT_THAT(SetProviderProperty(0, NCRYPT_IMPL_TYPE_PROPERTY, nullptr,
                                  sizeof(DWORD), 0),
              StatusSsIs(NTE_INVALID_HANDLE));
}

TEST(BridgeTest, SetProviderPropertyNameNull) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(
      SetProviderProperty(provider_handle, nullptr, nullptr, sizeof(DWORD), 0),
      StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, SetProviderPropertyInputNull) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(
      SetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY, nullptr,
                          sizeof(DWORD), NCRYPT_PERSIST_ONLY_FLAG),
      StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, SetProviderPropertyInvalidName) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD input = 1337;
  EXPECT_THAT(
      SetProviderProperty(provider_handle, NCRYPT_UI_POLICY_PROPERTY,
                          reinterpret_cast<uint8_t*>(&input), sizeof(DWORD), 0),
      StatusSsIs(NTE_NOT_SUPPORTED));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, SetProviderPropertyImmutableProperty) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD input = NCRYPT_IMPL_SOFTWARE_FLAG;
  EXPECT_THAT(
      SetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY,
                          reinterpret_cast<uint8_t*>(&input), sizeof(input), 0),
      StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory and shut down logging.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, SetProviderPropertyInvalidFlag) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  DWORD input = NCRYPT_IMPL_SOFTWARE_FLAG;
  EXPECT_THAT(SetProviderProperty(provider_handle, NCRYPT_IMPL_TYPE_PROPERTY,
                                  reinterpret_cast<uint8_t*>(&input),
                                  sizeof(DWORD), NCRYPT_PERSIST_ONLY_FLAG),
              StatusSsIs(NTE_BAD_FLAGS));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, OpenKeySuccess) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();

  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  NCRYPT_KEY_HANDLE key_handle;
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));
  EXPECT_NE(key_handle, 0);

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, OpenKeyInvalidHandle) {
  EXPECT_THAT(OpenKey(0, nullptr, L"some_key_name", 0, 0),
              StatusSsIs(NTE_INVALID_HANDLE));
}

TEST(BridgeTest, OpenKeyInvalidOutputHandle) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(OpenKey(provider_handle, nullptr, L"some_key_name", 0, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, OpenKeyInvalidName) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  NCRYPT_KEY_HANDLE key_handle;
  EXPECT_THAT(OpenKey(provider_handle, &key_handle, nullptr, 0, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, OpenKeyInvalidLegacyKeySpec) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  NCRYPT_KEY_HANDLE key_handle;
  EXPECT_THAT(OpenKey(provider_handle, &key_handle, L"some_key_name", 0, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, OpenKeyInvalidFlag) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  NCRYPT_KEY_HANDLE key_handle;
  EXPECT_THAT(OpenKey(provider_handle, &key_handle, L"some_key_name",
                      AT_SIGNATURE, NCRYPT_PERSIST_ONLY_FLAG),
              StatusSsIs(NTE_BAD_FLAGS));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, OpenKeyNotFound) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  std::string invalid_key_name = ckv.name();
  invalid_key_name[invalid_key_name.length() - 1] = '2';
  EXPECT_THAT(
      OpenKey(reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider), &key_handle,
              StringToWide(invalid_key_name).data(), AT_SIGNATURE, 0),
      StatusSsIs(NTE_BAD_KEYSET));
}

TEST(BridgeTest, OpenKeyInvalidAlgorithm) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();

  kms_v1::CryptoKeyVersion ckv =
      NewCryptoKeyVersion(client.get(), kms_v1::CryptoKey::ASYMMETRIC_SIGN,
                          kms_v1::CryptoKeyVersion::RSA_SIGN_RAW_PKCS1_2048,
                          kms_v1::ProtectionLevel::HSM);

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  EXPECT_THAT(
      OpenKey(reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider), &key_handle,
              StringToWide(ckv.name()).data(), AT_SIGNATURE, 0),
      StatusSsIs(NTE_NOT_SUPPORTED));
}

TEST(BridgeTest, OpenKeyInvalidProtectionLevel) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();

  kms_v1::CryptoKeyVersion ckv =
      NewCryptoKeyVersion(client.get(), kms_v1::CryptoKey::ASYMMETRIC_SIGN,
                          kms_v1::CryptoKeyVersion::EC_SIGN_P256_SHA256,
                          kms_v1::ProtectionLevel::SOFTWARE);

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  EXPECT_THAT(
      OpenKey(reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider), &key_handle,
              StringToWide(ckv.name()).data(), AT_SIGNATURE, 0),
      StatusSsIs(NTE_NOT_SUPPORTED));
}

TEST(BridgeTest, FreeKeySuccess) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  NCRYPT_KEY_HANDLE key_handle;
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, FreeKeyInvalidProviderHandle) {
  EXPECT_THAT(FreeKey(0, 0), StatusSsIs(NTE_INVALID_HANDLE));
}

TEST(BridgeTest, FreeKeyInvalidKeyHandle) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(FreeKey(provider_handle, 0), StatusSsIs(NTE_INVALID_HANDLE));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, FreeKeyInvalidHandleCombination) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  NCRYPT_KEY_HANDLE key_handle;
  std::wstring key_name = StringToWide(ckv.name());
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    const_cast<PWSTR>(key_name.data()), AT_SIGNATURE, 0));

  // Get new provider handle, unrelated to the key opened previously.
  NCRYPT_PROV_HANDLE other_provider_handle;
  EXPECT_OK(OpenProvider(&other_provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(FreeKey(other_provider_handle, key_handle),
              StatusSsIs(NTE_INVALID_HANDLE));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
  EXPECT_OK(FreeProvider(other_provider_handle));
}

TEST(BridgeTest, GetKeyPropertyGetSizeSuccess) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  DWORD output_size = 0;
  EXPECT_OK(GetKeyProperty(provider_handle, key_handle,
                           NCRYPT_KEY_USAGE_PROPERTY, nullptr, sizeof(DWORD),
                           &output_size, 0));
  EXPECT_EQ(output_size, sizeof(DWORD));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, GetKeyDwordPropertySuccess) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  DWORD output = 0;
  DWORD output_size = 0;
  EXPECT_OK(GetKeyProperty(
      provider_handle, key_handle, NCRYPT_KEY_USAGE_PROPERTY,
      reinterpret_cast<uint8_t*>(&output), sizeof(output), &output_size, 0));
  EXPECT_EQ(output_size, sizeof(output));
  EXPECT_EQ(output, NCRYPT_ALLOW_SIGNING_FLAG);

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, GetKeyWstringPropertySuccess) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  DWORD output_size = 0;
  // Get property size.
  EXPECT_OK(GetKeyProperty(provider_handle, key_handle,
                           NCRYPT_ALGORITHM_PROPERTY, nullptr, 0, &output_size,
                           0));

  std::vector<uint8_t> output(output_size);
  EXPECT_OK(GetKeyProperty(provider_handle, key_handle,
                           NCRYPT_ALGORITHM_PROPERTY, output.data(),
                           output.size(), &output_size, 0));
  EXPECT_EQ(output_size, output.size());
  EXPECT_EQ(std::wstring(output.begin(), output.end()),
            BCRYPT_ECDSA_P256_ALGORITHM);

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, GetKeyPropertyInvalidProviderHandle) {
  EXPECT_THAT(GetKeyProperty(0, 0, nullptr, nullptr, 0, nullptr, 0),
              StatusSsIs(NTE_INVALID_HANDLE));
}

TEST(BridgeTest, GetKeyPropertyInvalidKeyHandle) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(
      GetKeyProperty(provider_handle, 0, nullptr, nullptr, 0, nullptr, 0),
      StatusSsIs(NTE_INVALID_HANDLE));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, GetKeyPropertyNameNull) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  EXPECT_THAT(GetKeyProperty(provider_handle, key_handle, nullptr, nullptr, 0,
                             nullptr, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, GetKeyPropertyOutputBufferNull) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  EXPECT_THAT(GetKeyProperty(provider_handle, key_handle,
                             NCRYPT_KEY_USAGE_PROPERTY, nullptr, 0, nullptr, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, GetKeyPropertyInvalidFlag) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  uint8_t output;
  DWORD output_size;
  EXPECT_THAT(GetKeyProperty(provider_handle, key_handle,
                             NCRYPT_KEY_USAGE_PROPERTY, &output, sizeof(DWORD),
                             &output_size, NCRYPT_PERSIST_ONLY_FLAG),
              StatusSsIs(NTE_BAD_FLAGS));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, GetKeyPropertyInvalidName) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  uint8_t output;
  DWORD output_size;
  EXPECT_THAT(
      GetKeyProperty(provider_handle, key_handle, NCRYPT_UI_POLICY_PROPERTY,
                     &output, sizeof(DWORD), &output_size, 0),
      StatusSsIs(NTE_NOT_SUPPORTED));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, GetKeyPropertyOutputBufferTooShort) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  uint8_t output;
  DWORD output_size;
  EXPECT_THAT(
      GetKeyProperty(provider_handle, key_handle, NCRYPT_KEY_USAGE_PROPERTY,
                     &output, 1, &output_size, 0),
      StatusSsIs(NTE_BUFFER_TOO_SMALL));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, SignHashGetSignatureSizeSuccess) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  std::vector<uint8_t> digest(32, '\1');
  DWORD output_size = 0;
  EXPECT_OK(SignHash(provider_handle, key_handle, nullptr, digest.data(),
                     digest.size(), nullptr, 0, &output_size, 0));
  EXPECT_EQ(output_size, 64);

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, SignHashSuccess) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  std::vector<uint8_t> digest(32, '\1');
  std::vector<uint8_t> signature(64);
  DWORD output_size = 0;
  EXPECT_OK(SignHash(provider_handle, key_handle, nullptr, digest.data(),
                     digest.size(), signature.data(), signature.size(),
                     &output_size, 0));

  ASSERT_OK_AND_ASSIGN(Object * object,
                       ValidateKeyHandle(provider_handle, key_handle));
  EXPECT_OK(EcdsaVerifyP1363(
      object->ec_public_key(), EVP_sha256(),
      absl::MakeConstSpan(digest.data(), digest.size()),
      absl::MakeConstSpan(signature.data(), signature.size())));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, SignHashInvalidProviderHandle) {
  EXPECT_THAT(SignHash(0, 0, nullptr, nullptr, 0, nullptr, 0, nullptr, 0),
              StatusSsIs(NTE_INVALID_HANDLE));
}

TEST(BridgeTest, SignHashInvalidKeyHandle) {
  NCRYPT_PROV_HANDLE provider_handle;
  EXPECT_OK(OpenProvider(&provider_handle, kProviderName.data(), 0));

  EXPECT_THAT(
      SignHash(provider_handle, 0, nullptr, nullptr, 0, nullptr, 0, nullptr, 0),
      StatusSsIs(NTE_INVALID_HANDLE));

  // Clean up memory.
  EXPECT_OK(FreeProvider(provider_handle));
}

TEST(BridgeTest, SignHashPaddingInfoNotNull) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  DWORD padding_info = 1337;
  EXPECT_THAT(SignHash(provider_handle, key_handle, &padding_info, nullptr, 0,
                       nullptr, 0, nullptr, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, SignHashInputDigestNull) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  EXPECT_THAT(SignHash(provider_handle, key_handle, nullptr, nullptr, 0,
                       nullptr, 0, nullptr, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, SignHashOutputLengthBufferNull) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  std::vector<uint8_t> digest(32, '\1');
  EXPECT_THAT(SignHash(provider_handle, key_handle, nullptr, digest.data(),
                       digest.size(), nullptr, 0, nullptr, 0),
              StatusSsIs(NTE_INVALID_PARAMETER));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

TEST(BridgeTest, SignHashInvalidFlag) {
  ASSERT_OK_AND_ASSIGN(auto fake_server, fakekms::Server::New());
  auto client = fake_server->NewClient();
  kms_v1::CryptoKeyVersion ckv = NewCryptoKeyVersion(client.get());

  Provider provider;
  SetFakeKmsProviderProperties(&provider, fake_server->listen_addr());

  NCRYPT_KEY_HANDLE key_handle;
  NCRYPT_PROV_HANDLE provider_handle =
      reinterpret_cast<NCRYPT_PROV_HANDLE>(&provider);
  EXPECT_OK(OpenKey(provider_handle, &key_handle,
                    StringToWide(ckv.name()).data(), AT_SIGNATURE, 0));

  uint8_t output;
  DWORD output_size;
  std::vector<uint8_t> digest(32, '\1');
  EXPECT_THAT(SignHash(provider_handle, key_handle, nullptr, digest.data(),
                       digest.size(), &output, 0, &output_size,
                       NCRYPT_PERSIST_ONLY_FLAG),
              StatusSsIs(NTE_BAD_FLAGS));

  // Clean up memory.
  EXPECT_OK(FreeKey(provider_handle, key_handle));
}

}  // namespace
}  // namespace cloud_kms::kmscng
