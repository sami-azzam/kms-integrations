package contract

import (
	"context"
	"testing"

	"oss-tools/kmsp11/test/fakekms/testutil"

	"github.com/google/go-cmp/cmp"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	kmspb "google.golang.org/genproto/googleapis/cloud/kms/v1"
)

func TestGetCryptoKeyEqualsCreated(t *testing.T) {
	ctx := context.Background()
	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})

	want := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent:      kr.Name,
		CryptoKeyId: "foo",
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_ENCRYPT_DECRYPT,
		},
		SkipInitialVersionCreation: true,
	})

	got, err := client.GetCryptoKey(ctx, &kmspb.GetCryptoKeyRequest{
		Name: want.Name,
	})
	if err != nil {
		t.Error(err)
	}

	if diff := cmp.Diff(want, got, testutil.ProtoDiffOpts()...); diff != "" {
		t.Errorf("proto mismatch (-want +got): %s", diff)
	}
}

func TestGetCryptoKeyMalformedName(t *testing.T) {
	ctx := context.Background()

	_, err := client.GetCryptoKey(ctx, &kmspb.GetCryptoKeyRequest{
		Name: "malformed name",
	})
	if status.Code(err) != codes.InvalidArgument {
		t.Errorf("err=%v, want code=%s", err, codes.InvalidArgument)
	}
}

func TestGetCryptoKeyNotFound(t *testing.T) {
	ctx := context.Background()
	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})

	_, err := client.GetCryptoKey(ctx, &kmspb.GetCryptoKeyRequest{
		Name: kr.Name + "/cryptoKeys/foo",
	})
	if status.Code(err) != codes.NotFound {
		t.Errorf("err=%v, want code=%s", err, codes.NotFound)
	}
}
