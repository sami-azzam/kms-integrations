package contract

import (
	"context"
	"testing"

	"github.com/google/go-cmp/cmp"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	kmspb "google.golang.org/genproto/googleapis/cloud/kms/v1"
)

func TestListCryptoKeysSorted(t *testing.T) {
	ctx := context.Background()

	kr := client.CreateTestKR(ctx, t, &kmspb.CreateKeyRingRequest{Parent: location})

	ckb := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent:      kr.Name,
		CryptoKeyId: "key-b",
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_ENCRYPT_DECRYPT,
		},
	})

	cka := client.CreateTestCK(ctx, t, &kmspb.CreateCryptoKeyRequest{
		Parent:      kr.Name,
		CryptoKeyId: "key-a",
		CryptoKey: &kmspb.CryptoKey{
			Purpose: kmspb.CryptoKey_ENCRYPT_DECRYPT,
		},
	})

	iter := client.ListCryptoKeys(ctx, &kmspb.ListCryptoKeysRequest{Parent: kr.Name})

	r1, err := iter.Next()
	if err != nil {
		t.Fatalf("first call to iter.Next() resulted in error=%v, want nil", err)
	}
	if diff := cmp.Diff(cka, r1, ProtoDiffOpts()...); diff != "" {
		t.Errorf("first element mismatch (-want +got): %s", diff)
	}

	r2, err := iter.Next()
	if err != nil {
		t.Fatalf("second call to iter.Next() resulted in error=%v, want nil", err)
	}
	if diff := cmp.Diff(ckb, r2, ProtoDiffOpts()...); diff != "" {
		t.Errorf("second element mismatch (-want +got): %s", diff)
	}
}

func TestListCryptoKeysMalformedParent(t *testing.T) {
	ctx := context.Background()

	iter := client.ListCryptoKeys(ctx, &kmspb.ListCryptoKeysRequest{
		Parent: "locations/foo",
	})
	if _, err := iter.Next(); status.Code(err) != codes.InvalidArgument {
		t.Errorf("err=%v, want code=%s", err, codes.InvalidArgument)
	}
}
