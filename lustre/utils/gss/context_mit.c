/*
  Copyright (c) 2004-2006 The Regents of the University of Michigan.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
  3. Neither the name of the University nor the names of its
     contributors may be used to endorse or promote products derived
     from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

#ifndef HAVE_LUCID_CONTEXT_SUPPORT
#ifdef HAVE_KRB5

#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <gssapi/gssapi.h>
#include <rpc/rpc.h>
#include <rpc/auth_gss.h>
#include "gss_util.h"
#include "gss_oids.h"
#include "err_util.h"
#include "context.h"

#include <krb5.h>

#if (KRB5_VERSION > 131)
/* XXX argggg, there's gotta be a better way than just duplicating this
 * whole struct.  Unfortunately, this is in a "private" header file,
 * so this is our best choice at this point :-/
 */

typedef struct _krb5_gss_ctx_id_rec {
   unsigned int initiate : 1;   /* nonzero if initiating, zero if accepting */
   unsigned int established : 1;
   unsigned int big_endian : 1;
   unsigned int have_acceptor_subkey : 1;
   unsigned int seed_init : 1;  /* XXX tested but never actually set */
#ifdef CFX_EXERCISE
   unsigned int testing_unknown_tokid : 1; /* for testing only */
#endif
   OM_uint32 gss_flags;
   unsigned char seed[16];
   krb5_principal here;
   krb5_principal there;
   krb5_keyblock *subkey;
   int signalg;
   size_t cksum_size;
   int sealalg;
   krb5_keyblock *enc;
   krb5_keyblock *seq;
   krb5_timestamp endtime;
   krb5_flags krb_flags;
   /* XXX these used to be signed.  the old spec is inspecific, and
      the new spec specifies unsigned.  I don't believe that the change
      affects the wire encoding. */
   uint64_t seq_send;		/* gssint_uint64 */
   uint64_t seq_recv;		/* gssint_uint64 */
   void *seqstate;
   krb5_auth_context auth_context;
   gss_OID_desc *mech_used;	/* gss_OID_desc */
    /* Protocol spec revision
       0 => RFC 1964 with 3DES and RC4 enhancements
       1 => draft-ietf-krb-wg-gssapi-cfx-01
       No others defined so far.  */
   int proto;
   krb5_cksumtype cksumtype;    /* for "main" subkey */
   krb5_keyblock *acceptor_subkey; /* CFX only */
   krb5_cksumtype acceptor_subkey_cksumtype;
#ifdef CFX_EXERCISE
    gss_buffer_desc init_token;
#endif
} krb5_gss_ctx_id_rec, *krb5_gss_ctx_id_t;

#else	/* KRB5_VERSION > 131 */

typedef struct _krb5_gss_ctx_id_rec {
	int initiate;
	u_int32_t gss_flags;
	int seed_init;
	unsigned char seed[16];
	krb5_principal here;
	krb5_principal there;
	krb5_keyblock *subkey;
	int signalg;
	int cksum_size;
	int sealalg;
	krb5_keyblock *enc;
	krb5_keyblock *seq;
	krb5_timestamp endtime;
	krb5_flags krb_flags;
	krb5_ui_4 seq_send;
	krb5_ui_4 seq_recv;
	void *seqstate;
	int established;
	int big_endian;
	krb5_auth_context auth_context;
	gss_OID_desc *mech_used;
	int nctypes;
	krb5_cksumtype *ctypes;
} krb5_gss_ctx_id_rec, *krb5_gss_ctx_id_t;

#endif /* KRB5_VERSION */


static int
write_keyblock(char **p, char *end, struct _krb5_keyblock *arg)
{
	gss_buffer_desc tmp;

	if (WRITE_BYTES(p, end, arg->enctype)) return -1;
	tmp.length = arg->length;
	tmp.value = arg->contents;
	if (write_buffer(p, end, &tmp)) return -1;
	return 0;
}

/*
 * XXX Hack alert! XXX Do NOT submit upstream!
 * XXX Hack alert! XXX Do NOT submit upstream!
 *
 * We shouldn't be using these definitions
 *
 * XXX Hack alert! XXX Do NOT submit upstream!
 * XXX Hack alert! XXX Do NOT submit upstream!
 */
/* for 3DES */
#define KG_USAGE_SEAL 22
#define KG_USAGE_SIGN 23
#define KG_USAGE_SEQ  24

/* for rfc???? */
#define KG_USAGE_ACCEPTOR_SEAL  22
#define KG_USAGE_ACCEPTOR_SIGN  23
#define KG_USAGE_INITIATOR_SEAL 24
#define KG_USAGE_INITIATOR_SIGN 25

/* Lifted from mit src/lib/gssapi/krb5/gssapiP_krb5.h */
enum seal_alg {
  SEAL_ALG_NONE            = 0xffff,
  SEAL_ALG_DES             = 0x0000,
  SEAL_ALG_1               = 0x0001, /* not published */
  SEAL_ALG_MICROSOFT_RC4   = 0x0010, /* microsoft w2k;  */
  SEAL_ALG_DES3KD          = 0x0002
};

#define KEY_USAGE_SEED_ENCRYPTION	0xAA
#define KEY_USAGE_SEED_INTEGRITY	0x55
#define KEY_USAGE_SEED_CHECKSUM		0x99
#define K5CLENGTH 5

extern void krb5_enc_des3;
extern void krb5int_enc_des3;
extern void krb5int_enc_arcfour;
extern void krb5int_enc_aes128;
extern void krb5int_enc_aes256;
extern int krb5_derive_key();

/*
 * XXX Hack alert! XXX Do NOT submit upstream!
 * XXX Hack alert! XXX Do NOT submit upstream!
 *
 * We should be passing down a single key to the kernel
 * and it should be deriving the other keys.  We cannot
 * depend on any of this stuff being accessible in the
 * future.
 *
 * XXX Hack alert! XXX Do NOT submit upstream!
 * XXX Hack alert! XXX Do NOT submit upstream!
 */
/*
 * Function to derive a new key from a given key and given constant data.
 */
static krb5_error_code
derive_key(const krb5_keyblock *in, krb5_keyblock *out, int usage, char extra)
{
	krb5_error_code code;
	unsigned char constant_data[K5CLENGTH];
	krb5_data datain;
	int keylength;
	void *enc;

	switch (in->enctype) {
#ifdef ENCTYPE_DES3_CBC_RAW
	case ENCTYPE_DES3_CBC_RAW:
		keylength = 24;
/* Extra hack, the structure was renamed as rc4 was added... */
#if defined(ENCTYPE_ARCFOUR_HMAC)
		enc = &krb5int_enc_des3;
#else
		enc = &krb5_enc_des3;
#endif
		break;
#endif
#ifdef ENCTYPE_ARCFOUR_HMAC
	case ENCTYPE_ARCFOUR_HMAC:
		keylength = 16;
		enc = &krb5int_enc_arcfour;
		break;
#endif
	default:
		code = KRB5_BAD_ENCTYPE;
		goto out;
	}

	/* allocate memory for output key */
	if ((out->contents = malloc(keylength)) == NULL) {
		code = ENOMEM;
		goto out;
	}
	out->length = keylength;
	out->enctype = in->enctype;

	datain.data = (char *) constant_data;
	datain.length = K5CLENGTH;

	datain.data[0] = (usage>>24)&0xff;
	datain.data[1] = (usage>>16)&0xff;
	datain.data[2] = (usage>>8)&0xff;
	datain.data[3] = usage&0xff;

	datain.data[4] = (char) extra;

	if ((code = krb5_derive_key(enc, in, out, &datain))) {
		free(out->contents);
		out->contents = NULL;
	}

  out:
  	if (code)
		printerr(0, "ERROR: derive_key returning error %d (%s)\n",
			 code, error_message(code));
	return (code);
}

/*
 * We really shouldn't know about glue-layer context structure, but
 * we need to get at the real krb5 context pointer.  This should be
 * removed as soon as we say there is no support for MIT Kerberos
 * prior to 1.4 -- which gives us "legal" access to the context info.
 */
typedef struct gss_union_ctx_id_t {
	gss_OID         mech_type;
	gss_ctx_id_t    internal_ctx_id;
} gss_union_ctx_id_desc, *gss_union_ctx_id_t;

int
serialize_krb5_ctx(gss_ctx_id_t ctx, gss_buffer_desc *buf)
{
	krb5_gss_ctx_id_t kctx = ((gss_union_ctx_id_t)ctx)->internal_ctx_id;
	char *p, *end;
	static int constant_zero = 0;
	static int constant_one = 1;
	static int constant_two = 2;
	uint32_t word_seq_send;
	u_int64_t seq_send_64bit;
	uint32_t v2_flags = 0;
	krb5_keyblock derived_key;
	uint32_t numkeys;

	if (!(buf->value = calloc(1, MAX_CTX_LEN)))
		goto out_err;
	p = buf->value;
	end = buf->value + MAX_CTX_LEN;

	switch (kctx->sealalg) {
	case SEAL_ALG_DES:
		/* Old format of context to the kernel */
		if (kctx->initiate) {
			if (WRITE_BYTES(&p, end, constant_one)) goto out_err;
		}
		else {
			if (WRITE_BYTES(&p, end, constant_zero)) goto out_err;
		}
		if (kctx->seed_init) {
			if (WRITE_BYTES(&p, end, constant_one)) goto out_err;
		}
		else {
			if (WRITE_BYTES(&p, end, constant_zero)) goto out_err;
		}
		if (write_bytes(&p, end, &kctx->seed, sizeof(kctx->seed)))
			goto out_err;
		if (WRITE_BYTES(&p, end, kctx->signalg)) goto out_err;
		if (WRITE_BYTES(&p, end, kctx->sealalg)) goto out_err;
		if (WRITE_BYTES(&p, end, kctx->endtime)) goto out_err;
		word_seq_send = kctx->seq_send;
		if (WRITE_BYTES(&p, end, word_seq_send)) goto out_err;
		if (write_oid(&p, end, kctx->mech_used)) goto out_err;

		printerr(2, "serialize_krb5_ctx: serializing keys with "
			 "enctype %d and length %d\n",
			 kctx->enc->enctype, kctx->enc->length);

		if (write_keyblock(&p, end, kctx->enc)) goto out_err;
		if (write_keyblock(&p, end, kctx->seq)) goto out_err;
		break;
	case SEAL_ALG_MICROSOFT_RC4:
	case SEAL_ALG_DES3KD:
		/* New format of context to the kernel */
		/* s32 endtime;
		 * u32 flags;
		 * #define KRB5_CTX_FLAG_INITIATOR        0x00000001
		 * #define KRB5_CTX_FLAG_CFX              0x00000002
		 * #define KRB5_CTX_FLAG_ACCEPTOR_SUBKEY  0x00000004
		 * u64 seq_send;
		 * u32  enctype;
		 * u32  size_of_each_key;    (  size in bytes )
		 * u32  number_of_keys;      (  N (assumed to be 3 for now) )
		 * keydata-1;                (  Ke  (Kenc for DES3) )
		 * keydata-2;                (  Ki  (Kseq for DES3) )
		 * keydata-3;                (  Kc (derived checksum key) )
		 */
		if (kctx->initiate) {
			if (WRITE_BYTES(&p, end, constant_one)) goto out_err;
		}
		else {
			if (WRITE_BYTES(&p, end, constant_zero)) goto out_err;
		}
		if (WRITE_BYTES(&p, end, kctx->endtime)) goto out_err;

		/* Only applicable flag for this is initiator */
		if (kctx->initiate) v2_flags |= KRB5_CTX_FLAG_INITIATOR;
		if (WRITE_BYTES(&p, end, v2_flags)) goto out_err;

		seq_send_64bit = kctx->seq_send;
		if (WRITE_BYTES(&p, end, seq_send_64bit)) goto out_err;

		if (WRITE_BYTES(&p, end, kctx->enc->enctype)) goto out_err;
		if (WRITE_BYTES(&p, end, kctx->enc->length)) goto out_err;
		numkeys = 3;
		if (WRITE_BYTES(&p, end, numkeys)) goto out_err;
		printerr(2, "serialize_krb5_ctx: serializing %d keys with "
			 "enctype %d and size %d\n",
			 numkeys, kctx->enc->enctype, kctx->enc->length);

		/* Ke */
		if (write_bytes(&p, end, kctx->enc->contents,
				kctx->enc->length))
			goto out_err;

		/* Ki */
		if (write_bytes(&p, end, kctx->enc->contents,
				kctx->enc->length))
			goto out_err;

		/* Kc */
		if (derive_key(kctx->seq, &derived_key,
			       KG_USAGE_SIGN, KEY_USAGE_SEED_CHECKSUM))
			goto out_err;
		if (write_bytes(&p, end, derived_key.contents,
				derived_key.length))
			goto out_err;
		free(derived_key.contents);
		break;
	default:
		printerr(0, "ERROR: serialize_krb5_ctx: unsupported seal "
			 "algorithm %d\n", kctx->sealalg);
		goto out_err;
	}

	buf->length = p - (char *)buf->value;
	return 0;

out_err:
	printerr(0, "ERROR: failed serializing krb5 context for kernel\n");
	if (buf->value) {
		free(buf->value);
	}
	buf->value = NULL;
	buf->length = 0;
	return -1;
}

#endif /* HAVE_KRB5 */
#endif /* HAVE_LUCID_CONTEXT_SUPPORT */
