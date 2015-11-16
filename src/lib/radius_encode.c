/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file radius.c
 * @brief Functions to encode RADIUS attributes
 *
 * @copyright 2000-2003,2006-2015  The FreeRADIUS server project
 */

#include <freeradius-devel/libradius.h>
#include <freeradius-devel/md5.h>

static unsigned int salt_offset = 0;

fr_thread_local_setup(uint8_t *, fr_radius_encode_value_hton_buff)

static ssize_t encode_value(uint8_t *out, size_t outlen,
			    RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			    char const *secret,
			    fr_dict_attr_t const **tlv_stack, int depth,
			    VALUE_PAIR const **pvp);

static ssize_t encode_rfc_hdr_internal(uint8_t *out, size_t outlen, RADIUS_PACKET const *packet,
				       RADIUS_PACKET const *original, char const *secret,
				       fr_dict_attr_t const **tlv_stack, unsigned int depth,
				       VALUE_PAIR const **pvp);

static ssize_t encode_tlv_hdr(uint8_t *out, size_t outlen,
			      RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			      char const *secret,
			      fr_dict_attr_t const **tlv_stack, unsigned int depth,
			      VALUE_PAIR const **pvp);

/** Encode a CHAP password
 *
 * @bug FIXME: might not work with Ascend because
 * we use vp->vp_length, and Ascend gear likes
 * to send an extra '\0' in the string!
 */
int fr_radius_encode_chap_password(uint8_t *output, RADIUS_PACKET *packet, int id, VALUE_PAIR *password)
{
	int		i;
	uint8_t		*ptr;
	uint8_t		string[MAX_STRING_LEN * 2 + 1];
	VALUE_PAIR	*challenge;

	/*
	 *	Sanity check the input parameters
	 */
	if ((packet == NULL) || (password == NULL)) return -1;

	/*
	 *	Note that the password VP can be EITHER
	 *	a User-Password attribute (from a check-item list),
	 *	or a CHAP-Password attribute (the client asking
	 *	the library to encode it).
	 */

	i = 0;
	ptr = string;
	*ptr++ = id;

	i++;
	memcpy(ptr, password->vp_strvalue, password->vp_length);
	ptr += password->vp_length;
	i += password->vp_length;

	/*
	 *	Use Chap-Challenge pair if present,
	 *	Request Authenticator otherwise.
	 */
	challenge = fr_pair_find_by_num(packet->vps, PW_CHAP_CHALLENGE, 0, TAG_ANY);
	if (challenge) {
		memcpy(ptr, challenge->vp_strvalue, challenge->vp_length);
		i += challenge->vp_length;
	} else {
		memcpy(ptr, packet->vector, AUTH_VECTOR_LEN);
		i += AUTH_VECTOR_LEN;
	}

	*output = id;
	fr_md5_calc((uint8_t *)output + 1, (uint8_t *)string, i);

	return 0;
}

/** Encode Tunnel-Password attributes when sending them out on the wire
 *
 * int *pwlen is updated to the new length of the encrypted
 * password - a multiple of 16 bytes.
 *
 * This is per RFC-2868 which adds a two char SALT to the initial intermediate
 * value MD5 hash.
 */
int fr_radius_encode_tunnel_password(char *passwd, size_t *pwlen, char const *secret, uint8_t const *vector)
{
	uint8_t		buffer[AUTH_VECTOR_LEN + MAX_STRING_LEN + 3];
	unsigned char	digest[AUTH_VECTOR_LEN];
	char		*salt;
	int		i, n, secretlen;
	unsigned	len, n2;

	len = *pwlen;

	if (len > 127) len = 127;

	/*
	 *	Shift the password 3 positions right to place a salt and original
	 *	length, tag will be added automatically on packet send.
	 */
	for (n = len ; n >= 0 ; n--) passwd[n + 3] = passwd[n];
	salt = passwd;
	passwd += 2;

	/*
	 *	save original password length as first password character;
	 */
	*passwd = len;
	len += 1;


	/*
	 *	Generate salt.  The RFC's say:
	 *
	 *	The high bit of salt[0] must be set, each salt in a
	 *	packet should be unique, and they should be random
	 *
	 *	So, we set the high bit, add in a counter, and then
	 *	add in some CSPRNG data.  should be OK..
	 */
	salt[0] = (0x80 | (((salt_offset++) & 0x0f) << 3) | (fr_rand() & 0x07));
	salt[1] = fr_rand();

	/*
	 *	Padd password to multiple of AUTH_PASS_LEN bytes.
	 */
	n = len % AUTH_PASS_LEN;
	if (n) {
		n = AUTH_PASS_LEN - n;
		for (; n > 0; n--, len++) passwd[len] = 0;
	}
	/* set new password length */
	*pwlen = len + 2;

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);
	memcpy(buffer, secret, secretlen);

	for (n2 = 0; n2 < len; n2 +=AUTH_PASS_LEN) {
		if (!n2) {
			memcpy(buffer + secretlen, vector, AUTH_VECTOR_LEN);
			memcpy(buffer + secretlen + AUTH_VECTOR_LEN, salt, 2);
			fr_md5_calc(digest, buffer, secretlen + AUTH_VECTOR_LEN + 2);
		} else {
			memcpy(buffer + secretlen, passwd + n2 - AUTH_PASS_LEN, AUTH_PASS_LEN);
			fr_md5_calc(digest, buffer, secretlen + AUTH_PASS_LEN);
		}
		for (i = 0; i < AUTH_PASS_LEN; i++) passwd[i + n2] ^= digest[i];
	}
	passwd[n2] = 0;
	return 0;
}

/** Encode password
 *
 * We assume that the passwd buffer passed is big enough.
 * RFC2138 says the password is max 128 chars, so the size
 * of the passwd buffer must be at least 129 characters.
 * Preferably it's just MAX_STRING_LEN.
 *
 * int *pwlen is updated to the new length of the encrypted
 * password - a multiple of 16 bytes.
 */
int fr_radius_encode_password(char *passwd, size_t *pwlen, char const *secret, uint8_t const *vector)
{
	FR_MD5_CTX	context, old;
	uint8_t		digest[AUTH_VECTOR_LEN];
	int		i, n, secretlen;
	int		len;

	/*
	 *	RFC maximum is 128 bytes.
	 *
	 *	If length is zero, pad it out with zeros.
	 *
	 *	If the length isn't aligned to 16 bytes,
	 *	zero out the extra data.
	 */
	len = *pwlen;

	if (len > 128) len = 128;

	if (len == 0) {
		memset(passwd, 0, AUTH_PASS_LEN);
		len = AUTH_PASS_LEN;
	} else if ((len % AUTH_PASS_LEN) != 0) {
		memset(&passwd[len], 0, AUTH_PASS_LEN - (len % AUTH_PASS_LEN));
		len += AUTH_PASS_LEN - (len % AUTH_PASS_LEN);
	}
	*pwlen = len;

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);

	fr_md5_init(&context);
	fr_md5_update(&context, (uint8_t const *) secret, secretlen);
	fr_md5_copy(&old, &context); /* save intermediate work */

	/*
	 *	Encrypt it in place.  Don't bother checking
	 *	len, as we've ensured above that it's OK.
	 */
	for (n = 0; n < len; n += AUTH_PASS_LEN) {
		if (n == 0) {
			fr_md5_update(&context, vector, AUTH_PASS_LEN);
			fr_md5_final(digest, &context);
		} else {
			fr_md5_copy(&context, &old);
			fr_md5_update(&context, (uint8_t *) passwd + n - AUTH_PASS_LEN, AUTH_PASS_LEN);
			fr_md5_final(digest, &context);
		}

		for (i = 0; i < AUTH_PASS_LEN; i++) passwd[i + n] ^= digest[i];
	}

	return 0;
}

static void encode_password(uint8_t *out, ssize_t *outlen, uint8_t const *input, size_t inlen,
			    char const *secret, uint8_t const *vector)
{
	FR_MD5_CTX	context, old;
	uint8_t		digest[AUTH_VECTOR_LEN];
	uint8_t		passwd[MAX_PASS_LEN];
	size_t		i, n;
	size_t		len;

	/*
	 *	If the length is zero, round it up.
	 */
	len = inlen;

	if (len > MAX_PASS_LEN) len = MAX_PASS_LEN;

	memcpy(passwd, input, len);
	if (len < sizeof(passwd)) memset(passwd + len, 0, sizeof(passwd) - len);

	if (len == 0) len = AUTH_PASS_LEN;
	else if ((len & 0x0f) != 0) {
		len += 0x0f;
		len &= ~0x0f;
	}
	*outlen = len;

	fr_md5_init(&context);
	fr_md5_update(&context, (uint8_t const *) secret, strlen(secret));
	fr_md5_copy(&old, &context);

	/*
	 *	Do first pass.
	 */
	fr_md5_update(&context, vector, AUTH_PASS_LEN);

	for (n = 0; n < len; n += AUTH_PASS_LEN) {
		if (n > 0) {
			fr_md5_copy(&context, &old);
			fr_md5_update(&context, passwd + n - AUTH_PASS_LEN, AUTH_PASS_LEN);
		}

		fr_md5_final(digest, &context);
		for (i = 0; i < AUTH_PASS_LEN; i++) passwd[i + n] ^= digest[i];
	}

	memcpy(out, passwd, len);
}


static void encode_tunnel_password(uint8_t *out, ssize_t *outlen,
				   uint8_t const *input, size_t inlen, size_t freespace,
				   char const *secret, uint8_t const *vector)
{
	FR_MD5_CTX	context, old;
	uint8_t		digest[AUTH_VECTOR_LEN];
	size_t		i, n;
	size_t		encrypted_len;

	/*
	 *	The password gets encoded with a 1-byte "length"
	 *	field.  Ensure that it doesn't overflow.
	 */
	if (freespace > 253) freespace = 253;

	/*
	 *	Limit the maximum size of the input password.  2 bytes
	 *	are taken up by the salt, and one by the encoded
	 *	"length" field.  Note that if we have a tag, the
	 *	"freespace" will be 252 octets, not 253 octets.
	 */
	if (inlen > (freespace - 3)) inlen = freespace - 3;

	/*
	 *	Length of the encrypted data is the clear-text
	 *	password length plus one byte which encodes the length
	 *	of the password.  We round up to the nearest encoding
	 *	block.  Note that this can result in the encoding
	 *	length being more than 253 octets.
	 */
	encrypted_len = inlen + 1;
	if ((encrypted_len & 0x0f) != 0) {
		encrypted_len += 0x0f;
		encrypted_len &= ~0x0f;
	}

	/*
	 *	We need 2 octets for the salt, followed by the actual
	 *	encrypted data.
	 */
	if (encrypted_len > (freespace - 2)) encrypted_len = freespace - 2;

	*outlen = encrypted_len + 2;	/* account for the salt */

	/*
	 *	Copy the password over, and zero-fill the remainder.
	 */
	memcpy(out + 3, input, inlen);
	memset(out + 3 + inlen, 0, *outlen - 3 - inlen);

	/*
	 *	Generate salt.  The RFCs say:
	 *
	 *	The high bit of salt[0] must be set, each salt in a
	 *	packet should be unique, and they should be random
	 *
	 *	So, we set the high bit, add in a counter, and then
	 *	add in some CSPRNG data.  should be OK..
	 */
	out[0] = (0x80 | (((salt_offset++) & 0x0f) << 3) | (fr_rand() & 0x07));
	out[1] = fr_rand();
	out[2] = inlen;	/* length of the password string */

	fr_md5_init(&context);
	fr_md5_update(&context, (uint8_t const *) secret, strlen(secret));
	fr_md5_copy(&old, &context);

	fr_md5_update(&context, vector, AUTH_VECTOR_LEN);
	fr_md5_update(&context, &out[0], 2);

	for (n = 0; n < encrypted_len; n += AUTH_PASS_LEN) {
		size_t block_len;

		if (n > 0) {
			fr_md5_copy(&context, &old);
			fr_md5_update(&context, out + 2 + n - AUTH_PASS_LEN, AUTH_PASS_LEN);
		}
		fr_md5_final(digest, &context);

		if ((2 + n + AUTH_PASS_LEN) < freespace) {
			block_len = AUTH_PASS_LEN;
		} else {
			block_len = freespace - 2 - n;
		}

		for (i = 0; i < block_len; i++) out[i + 2 + n] ^= digest[i];
	}
}

/** Converts vp_data to network byte order
 *
 * Provide a pointer to a buffer which contains the value of the VALUE_PAIR
 * in an architecture independent format.
 *
 * The pointer is only guaranteed to be valid between calls to fr_radius_encode_value_hton, and so long
 * as the source VALUE_PAIR is not freed.
 *
 * @param out where to write the pointer to the value.
 * @param vp to get the value from.
 * @return
 *	- The length of the value.
 *	- -1 on failure.
 */
ssize_t fr_radius_encode_value_hton(uint8_t const **out, VALUE_PAIR const *vp)
{
	uint8_t		*buffer;
	uint32_t	lvalue;
	uint64_t	lvalue64;

	*out = NULL;

	buffer = fr_thread_local_init(fr_radius_encode_value_hton_buff, free);
	if (!buffer) {
		int ret;

		buffer = malloc(sizeof(uint8_t) * sizeof(value_data_t));
		if (!buffer) {
			fr_strerror_printf("Failed allocating memory for fr_radius_encode_value_hton buffer");
			return -1;
		}

		ret = fr_thread_local_set(fr_radius_encode_value_hton_buff, buffer);
		if (ret != 0) {
			fr_strerror_printf("Failed setting up TLS for fr_radius_encode_value_hton buffer: %s", strerror(errno));
			free(buffer);
			return -1;
		}
	}

	VERIFY_VP(vp);

	switch (vp->da->type) {
	case PW_TYPE_STRING:
	case PW_TYPE_OCTETS:
		memcpy(out, &vp->data.ptr, sizeof(*out));
		break;

	/*
	 *	All of these values are at the same location.
	 */
	case PW_TYPE_IFID:
	case PW_TYPE_IPV4_ADDR:
	case PW_TYPE_IPV6_ADDR:
	case PW_TYPE_IPV6_PREFIX:
	case PW_TYPE_IPV4_PREFIX:
	case PW_TYPE_ABINARY:
	case PW_TYPE_ETHERNET:
	case PW_TYPE_COMBO_IP_ADDR:
	case PW_TYPE_COMBO_IP_PREFIX:
	{
		void const *p = &vp->data;
		memcpy(out, &p, sizeof(*out));
		break;
	}

	case PW_TYPE_BOOLEAN:
		buffer[0] = vp->vp_byte & 0x01;
		*out = buffer;
		break;

	case PW_TYPE_BYTE:
		buffer[0] = vp->vp_byte & 0xff;
		*out = buffer;
		break;

	case PW_TYPE_SHORT:
		buffer[0] = (vp->vp_short >> 8) & 0xff;
		buffer[1] = vp->vp_short & 0xff;
		*out = buffer;
		break;

	case PW_TYPE_INTEGER:
		lvalue = htonl(vp->vp_integer);
		memcpy(buffer, &lvalue, sizeof(lvalue));
		*out = buffer;
		break;

	case PW_TYPE_INTEGER64:
		lvalue64 = htonll(vp->vp_integer64);
		memcpy(buffer, &lvalue64, sizeof(lvalue64));
		*out = buffer;
		break;

	case PW_TYPE_DATE:
		lvalue = htonl(vp->vp_date);
		memcpy(buffer, &lvalue, sizeof(lvalue));
		*out = buffer;
		break;

	case PW_TYPE_SIGNED:
	{
		int32_t slvalue = htonl(vp->vp_signed);
		memcpy(buffer, &slvalue, sizeof(slvalue));
		*out = buffer;
		break;
	}

	case PW_TYPE_INVALID:
	case PW_TYPE_EXTENDED:
	case PW_TYPE_LONG_EXTENDED:
	case PW_TYPE_EVS:
	case PW_TYPE_VSA:
	case PW_TYPE_VENDOR:
	case PW_TYPE_TLV:
	case PW_TYPE_TIMEVAL:
	case PW_TYPE_DECIMAL:
	case PW_TYPE_MAX:
		fr_strerror_printf("Cannot get data for VALUE_PAIR type %i", vp->da->type);
		return -1;

	/* Don't add default */
	}

	return vp->vp_length;
}

static ssize_t encode_tlv_hdr_internal(uint8_t *out, size_t outlen,
				       RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
				       char const *secret,
				       fr_dict_attr_t const **tlv_stack, unsigned int depth,
				       VALUE_PAIR const **pvp)
{
	ssize_t			len;
	uint8_t			*p = out;
	VALUE_PAIR const	*vp = *pvp;
	fr_dict_attr_t const	*da = tlv_stack[depth];

	while (outlen >= 5) {
		FR_PROTO_STACK_PRINT(tlv_stack, depth);

		/*
		 *	Determine the nested type and call the appropriate encoder
		 */
		if (tlv_stack[depth + 1]->type == PW_TYPE_TLV) {
			len = encode_tlv_hdr(p, outlen, packet, original,
					     secret, tlv_stack, depth + 1, pvp);
		} else {
			len = encode_rfc_hdr_internal(p, outlen, packet, original,
						      secret, tlv_stack, depth + 1, pvp);
		}

		if (len < 0) return len;
		if (len == 0) return out[1];		/* Insufficient space */

		p += len;
		outlen -= len;				/* Subtract from the buffer we have available */

		if (!*pvp || (vp == *pvp)) break;	/* If nothing updated the attribute, stop */

		/*
	 	 *	We can encode multiple sub TLVs, if after
	 	 *	rebuilding the TLV Stack, the attribute
	 	 *	at this depth is the same.
	 	 */
		if (da != tlv_stack[depth]) break;
		vp = *pvp;

		FR_PROTO_HEX_DUMP("Done TLV", out, p - out);
	}

	return p - out;
}

static ssize_t encode_tlv_hdr(uint8_t *out, size_t outlen,
			      RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			      char const *secret,
			      fr_dict_attr_t const **tlv_stack, unsigned int depth,
			      VALUE_PAIR const **pvp)
{
	ssize_t			len;
	VALUE_PAIR const	*vp = *pvp;

	VERIFY_VP(vp);
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	if (tlv_stack[depth]->type != PW_TYPE_TLV) {
		fr_strerror_printf("%s: Expected type \"tlv\" got \"%s\"", __FUNCTION__,
				   fr_int2str(dict_attr_types, tlv_stack[depth]->type, "?Unknown?"));
		return -1;
	}

	if (!tlv_stack[depth + 1]) {
		fr_strerror_printf("%s: Can't encode empty TLV", __FUNCTION__);
		return -1;
	}

	if (outlen < 5) return 0;

	/*
	 *	Encode the first level of TLVs
	 */
	out[0] = tlv_stack[depth]->attr & 0xff;
	out[1] = 2;	/* TLV header */

	len = encode_tlv_hdr_internal(out + 2, outlen - 2, packet, original, secret, tlv_stack, depth, pvp);
	if (len <= 0) return len;
	if (len > 253) return 0;

	out[1] += len;

	return out[1];
}

/** Encodes the data portion of an attribute
 *
 * @return
 *	- Length of the data portion.
 *	- -1 on failure.
 */
static ssize_t encode_value(uint8_t *out, size_t outlen,
			    RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			    char const *secret,
			    fr_dict_attr_t const **tlv_stack, int depth,
			    VALUE_PAIR const **pvp)
{
	uint32_t		lvalue;
	ssize_t			len;
	uint8_t	const		*data;
	uint8_t			*ptr = out;
	uint8_t			array[4];
	uint64_t		lvalue64;
	VALUE_PAIR const	*vp = *pvp;
	fr_dict_attr_t const	*da = tlv_stack[depth];

	VERIFY_VP(vp);
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	/*
	 *	It's a little weird to consider a TLV as a value,
	 *	but it seems to work OK.
	 */
	if (da->type == PW_TYPE_TLV) {
		return encode_tlv_hdr(out, outlen, packet, original, secret, tlv_stack, depth, pvp);
	}

	/*
	 *	If it's not a TLV, it should be a value type RFC
	 *	attribute make sure that it is.
	 */
	if (tlv_stack[depth + 1] != NULL) {
		fr_strerror_printf("%s: Encoding value but not at top of stack", __FUNCTION__);
		return -1;
	}

	if ((*pvp)->da != da) {
		fr_strerror_printf("%s: Top of stack does not match vp->da", __FUNCTION__);
		return -1;
	}

	switch (da->type) {
	case PW_TYPE_STRUCTURAL:
		fr_strerror_printf("%s: Called with structural type %s", __FUNCTION__,
				   fr_int2str(dict_attr_types, tlv_stack[depth]->type, "?Unknown?"));
		return -1;

	default:
		break;
	}

	/*
	 *	Set up the default sources for the data.
	 */
	len = vp->vp_length;

	switch (da->type) {
	case PW_TYPE_STRING:
	case PW_TYPE_OCTETS:
		data = vp->data.ptr;
		if (!data) {
			fr_strerror_printf("ERROR: Cannot encode NULL data");
			return -1;
		}
		break;

	case PW_TYPE_IFID:
	case PW_TYPE_IPV4_ADDR:
	case PW_TYPE_IPV6_ADDR:
	case PW_TYPE_IPV6_PREFIX:
	case PW_TYPE_IPV4_PREFIX:
	case PW_TYPE_ABINARY:
	case PW_TYPE_ETHERNET:	/* just in case */
		data = (uint8_t const *) &vp->data;
		break;

	case PW_TYPE_BYTE:
		len = 1;	/* just in case */
		array[0] = vp->vp_byte;
		data = array;
		break;

	case PW_TYPE_SHORT:
		len = 2;	/* just in case */
		array[0] = (vp->vp_short >> 8) & 0xff;
		array[1] = vp->vp_short & 0xff;
		data = array;
		break;

	case PW_TYPE_INTEGER:
		len = 4;	/* just in case */
		lvalue = htonl(vp->vp_integer);
		memcpy(array, &lvalue, sizeof(lvalue));
		data = array;
		break;

	case PW_TYPE_INTEGER64:
		len = 8;	/* just in case */
		lvalue64 = htonll(vp->vp_integer64);
		data = (uint8_t *) &lvalue64;
		break;

		/*
		 *  There are no tagged date attributes.
		 */
	case PW_TYPE_DATE:
		lvalue = htonl(vp->vp_date);
		data = (uint8_t const *) &lvalue;
		len = 4;	/* just in case */
		break;

	case PW_TYPE_SIGNED:
	{
		int32_t slvalue;

		len = 4;	/* just in case */
		slvalue = htonl(vp->vp_signed);
		memcpy(array, &slvalue, sizeof(slvalue));
		data = array;
		break;
	}

	default:		/* unknown type: ignore it */
		fr_strerror_printf("ERROR: Unknown attribute type %d", da->type);
		return -1;
	}

	/*
	 *	No data: skip it.
	 */
	if (len == 0) {
		*pvp = vp->next;
		fr_proto_tlv_stack_build(tlv_stack, *pvp ? (*pvp)->da : NULL);
		return 0;
	}

	/*
	 *	Bound the data to the calling size
	 */
	if (len > (ssize_t)outlen) len = outlen;

	/*
	 *	Encrypt the various password styles
	 *
	 *	Attributes with encrypted values MUST be less than
	 *	128 bytes long.
	 */
	switch (vp->da->flags.encrypt) {
	case FLAG_ENCRYPT_USER_PASSWORD:
		encode_password(ptr, &len, data, len, secret, packet->vector);
		break;

	case FLAG_ENCRYPT_TUNNEL_PASSWORD:
		lvalue = 0;
		if (da->flags.has_tag) lvalue = 1;

		/*
		 *	Check if there's enough freespace.  If there isn't,
		 *	we discard the attribute.
		 *
		 *	This is ONLY a problem if we have multiple VSA's
		 *	in one Vendor-Specific, though.
		 */
		if (outlen < (18 + lvalue)) return 0;

		switch (packet->code) {
		case PW_CODE_ACCESS_ACCEPT:
		case PW_CODE_ACCESS_REJECT:
		case PW_CODE_ACCESS_CHALLENGE:
		default:
			if (!original) {
				fr_strerror_printf("ERROR: No request packet, cannot encrypt %s attribute "
						   "in the vp.", da->name);
				return -1;
			}

			if (lvalue) ptr[0] = TAG_VALID(vp->tag) ? vp->tag : TAG_NONE;
			encode_tunnel_password(ptr + lvalue, &len, data, len,
					       outlen - lvalue, secret, original->vector);
			len += lvalue;
			break;
		case PW_CODE_ACCOUNTING_REQUEST:
		case PW_CODE_DISCONNECT_REQUEST:
		case PW_CODE_COA_REQUEST:
			ptr[0] = TAG_VALID(vp->tag) ? vp->tag : TAG_NONE;
			encode_tunnel_password(ptr + 1, &len, data, len, outlen - 1, secret, packet->vector);
			len += lvalue;
			break;
		}
		break;

		/*
		 *	The code above ensures that this attribute
		 *	always fits.
		 */
	case FLAG_ENCRYPT_ASCEND_SECRET:
		if (len != 16) return 0;
		fr_radius_make_secret(ptr, packet->vector, secret, data);
		len = AUTH_VECTOR_LEN;
		break;

	default:
		if (vp->da->flags.has_tag && TAG_VALID(vp->tag)) {
			if (vp->da->type == PW_TYPE_STRING) {
				if (len > ((ssize_t) (outlen - 1))) len = outlen - 1;
				ptr[0] = vp->tag;
				ptr++;
			} else if (vp->da->type == PW_TYPE_INTEGER) {
				array[0] = vp->tag;
			} /* else it can't be any other type */
		}
		memcpy(ptr, data, len);
		break;
	} /* switch over encryption flags */

	/*
	 *	Rebuilds the TLV stack for encoding the next attribute
	 */
	*pvp = (*pvp)->next;
	fr_proto_tlv_stack_build(tlv_stack, *pvp ? (*pvp)->da : NULL);

	return len + (ptr - out);
}

static ssize_t attr_shift(uint8_t const *start, uint8_t const *end,
			  uint8_t *ptr, int hdr_len, ssize_t len,
			  int flag_offset, int vsa_offset)
{
	int check_len = len - ptr[1];
	int total = len + hdr_len;

	/*
	 *	Pass 1: Check if the addition of the headers
	 *	overflows the available freespace.  If so, return
	 *	what we were capable of encoding.
	 */

	while (check_len > (255 - hdr_len)) {
		total += hdr_len;
		check_len -= (255 - hdr_len);
	}

	/*
	 *	Note that this results in a number of attributes maybe
	 *	being marked as "encoded", but which aren't in the
	 *	packet.  Oh well.  The solution is to fix the
	 *	"encode_value" function to take into account the header
	 *	lengths.
	 */
	if ((ptr + ptr[1] + total) > end) return (ptr + ptr[1]) - start;

	/*
	 *	Pass 2: Now that we know there's enough freespace,
	 *	re-arrange the data to form a set of valid
	 *	RADIUS attributes.
	 */
	while (1) {
		int sublen = 255 - ptr[1];

		if (len <= sublen) {
			break;
		}

		len -= sublen;
		memmove(ptr + 255 + hdr_len, ptr + 255, sublen);
		memmove(ptr + 255, ptr, hdr_len);
		ptr[1] += sublen;
		if (vsa_offset) ptr[vsa_offset] += sublen;
		ptr[flag_offset] |= 0x80;

		ptr += 255;
		ptr[1] = hdr_len;
		if (vsa_offset) ptr[vsa_offset] = 3;
	}

	ptr[1] += len;
	if (vsa_offset) ptr[vsa_offset] += len;

	return (ptr + ptr[1]) - start;
}

/** Encode an "extended" attribute
 *
 */
static int encode_extended_hdr(uint8_t *out, size_t outlen,
			       RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			       char const *secret,
			       fr_dict_attr_t const **tlv_stack, unsigned int depth,
			       VALUE_PAIR const **pvp)
{
	int			len;
	int			hdr_len;
	uint8_t			*start = out;
	VALUE_PAIR const	*vp = *pvp;

	VERIFY_VP(vp);
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	if ((tlv_stack[depth]->type != PW_TYPE_EXTENDED) && (tlv_stack[depth]->type != PW_TYPE_LONG_EXTENDED)) {
		fr_strerror_printf("%s : Called for non-extended attribute type %s",
				   __FUNCTION__, fr_int2str(dict_attr_types, tlv_stack[depth]->type, "?Unknown?"));
		return -1;
	}

	/*
	 *	Encode which extended attribute it is.
	 */
	out[0] = tlv_stack[depth++]->attr & 0xff;

	/*
	 *	Encode the header for "short" attributes
	 */
	if (!vp->da->flags.long_extended) {
		if (outlen < 3) return 0;

		out[1] = 3;
		out[2] = vp->da->attr & 0xff;

	} else {
		if (outlen < 4) return 0;

		out[1] = 4;
		out[2] = vp->da->attr & 0xff;
		out[3] = 0;	/* flags start off at zero */
	}

	/*
	 *	Only "flagged" attributes can be longer than one
	 *	attribute.  The caller should really take care of
	 *	this, too.
	 */
	if (!vp->da->flags.long_extended && (outlen > 255)) {
		outlen = 255;
	}

	/*
	 *	Handle EVS
	 */
	if (tlv_stack[depth]->type == PW_TYPE_EVS) {
		uint8_t *evs = out + out[1];

		if (outlen < (size_t) (out[1] + 5)) return 0;

		depth++;

		out[2] = 26;

		evs[0] = 0;	/* always zero */
		evs[1] = (vp->da->vendor >> 16) & 0xff;
		evs[2] = (vp->da->vendor >> 8) & 0xff;
		evs[3] = vp->da->vendor & 0xff;
		evs[4] = vp->da->attr & 0xff;

		out[1] += 5;

		evs[5] = tlv_stack[depth++]->attr & 0xff;
	}
	hdr_len = out[1];

	len = encode_value(out + out[1], outlen - hdr_len, packet, original, secret, tlv_stack, depth, pvp);
	if (len <= 0) return len;

	/*
	 *	There may be more than 252 octets of data encoded in
	 *	the attribute.  If so, move the data up in the packet,
	 *	and copy the existing header over.  Set the "M" flag ONLY
	 *	after copying the rest of the data.
	 */
	if (len > (255 - out[1])) {
		if (vp->da->flags.long_extended) {
			return attr_shift(start, start + outlen, out, 4, len, 3, 0);
		}

		len = (255 - out[1]); /* truncate to fit */
	}

	out[1] += len;

#ifndef NDEBUG
	if ((fr_debug_lvl > 3) && fr_log_fp) {
		int jump = 3;

		fprintf(fr_log_fp, "\t\t%02x %02x  ", out[0], out[1]);
		if (!vp->da->flags.long_extended) {
			fprintf(fr_log_fp, "%02x  ", out[2]);

		} else {
			fprintf(fr_log_fp, "%02x %02x  ", out[2], out[3]);
			jump = 4;
		}

		if (vp->da->flags.evs) {
			fprintf(fr_log_fp, "%02x%02x%02x%02x (%u)  %02x  ",
				out[jump], out[jump + 1],
				out[jump + 2], out[jump + 3],
				((out[jump + 1] << 16) |
				 (out[jump + 2] << 8) |
				 out[jump + 3]),
				out[jump + 4]);
			jump += 5;
		}

		FR_PROTO_HEX_DUMP("Done extended header", out + jump, len);
	}
#endif

	return (out + out[1]) - start;
}

/** Encode an RFC format attribute, with the "concat" flag set
 *
 * If there isn't enough freespace in the packet, the data is
 * truncated to fit.
 */
static ssize_t encode_concat(uint8_t *out, size_t outlen,
			     UNUSED RADIUS_PACKET const *packet, UNUSED RADIUS_PACKET const *original,
			     UNUSED char const *secret,
			     fr_dict_attr_t const **tlv_stack, UNUSED unsigned int depth,
			     VALUE_PAIR const **pvp)
{
	uint8_t			*ptr = out;
	uint8_t			const *p;
	size_t			len, left;
	VALUE_PAIR const	*vp = *pvp;

	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	p = vp->vp_octets;
	len = vp->vp_length;

	while (len > 0) {
		if (outlen <= 2) break;

		ptr[0] = tlv_stack[depth]->attr & 0xff;
		ptr[1] = 2;

		left = len;

		/* no more than 253 octets */
		if (left > 253) left = 253;

		/* no more than "freespace" octets */
		if (outlen < (left + 2)) left = outlen - 2;

		memcpy(ptr + 2, p, left);

#ifndef NDEBUG
		if ((fr_debug_lvl > 3) && fr_log_fp) {
			fprintf(fr_log_fp, "\t\t%02x %02x  ", ptr[0], ptr[1]);
			FR_PROTO_HEX_DUMP("Done concat", ptr + 2, len);
		}
#endif
		ptr[1] += left;
		ptr += ptr[1];
		p += left;
		outlen -= left;
		len -= left;
	}

	*pvp = vp->next;

	/*
	 *	@fixme: attributes with 'concat' MUST of type
	 *	'octets', and therefore CANNOT have any TLV data in them.
	 */
	fr_proto_tlv_stack_build(tlv_stack, *pvp ? (*pvp)->da : NULL);

	return ptr - out;
}

/** Encode an RFC format TLV.
 *
 * This could be a standard attribute, or a TLV data type.
 * If it's a standard attribute, then vp->da->attr == attribute.
 * Otherwise, attribute may be something else.
 */
static ssize_t encode_rfc_hdr_internal(uint8_t *out, size_t outlen, RADIUS_PACKET const *packet,
				       RADIUS_PACKET const *original, char const *secret,
				       fr_dict_attr_t const **tlv_stack, unsigned int depth,
				       VALUE_PAIR const **pvp)
{
	ssize_t len;

	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	switch (tlv_stack[depth]->type) {
	case PW_TYPE_STRUCTURAL:
		fr_strerror_printf("%s: Called with structural type %s", __FUNCTION__,
				   fr_int2str(dict_attr_types, tlv_stack[depth]->type, "?Unknown?"));
		return -1;

	default:
		if ((tlv_stack[depth]->attr == 0) || (tlv_stack[depth]->attr > 255)) {
			fr_strerror_printf("%s: Called with non-standard attribute %u", __FUNCTION__,
					   tlv_stack[depth]->attr);
			return -1;
		}
		break;
	}

	if (outlen <= 2) return 0;

	out[0] = tlv_stack[depth]->attr & 0xff;
	out[1] = 2;

	if (outlen > ((unsigned) 255 - out[1])) outlen = 255 - out[1];

	len = encode_value(out + out[1], outlen, packet, original, secret, tlv_stack, depth, pvp);
	if (len <= 0) return len;

	out[1] += len;

#ifndef NDEBUG
	if ((fr_debug_lvl > 3) && fr_log_fp) {
		fprintf(fr_log_fp, "\t\t%02x %02x  ", out[0], out[1]);
		FR_PROTO_HEX_DUMP("Done RFC header", out + 2, len);
	}
#endif

	return out[1];
}


/** Encode a VSA which is a TLV
 *
 * If it's in the RFC format, call encode_rfc_hdr_internal.  Otherwise, encode it here.
 */
static ssize_t encode_vendor_attr_hdr(uint8_t *out, size_t outlen,
				      RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
				      char const *secret,
				      fr_dict_attr_t const **tlv_stack, unsigned int depth,
				      VALUE_PAIR const **pvp)
{
	ssize_t			len;
	fr_dict_vendor_t	*dv;
	fr_dict_attr_t const	*da = tlv_stack[depth];

	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	/*
	 *	Unknown vendor: RFC format.
	 *	Known vendor and RFC format: go do that.
	 */
	dv = fr_dict_vendor_by_num(da->parent->attr);
	if (!dv || ((da->type != PW_TYPE_TLV) && (dv->type == 1) && (dv->length == 1))) {
		return encode_rfc_hdr_internal(out, outlen, packet, original, secret,
					       tlv_stack, depth, pvp);
	}

	/*
	 *	Vendors use different widths for their
	 *	attribute number fields.
	 */
	switch (dv->type) {
	default:
		fr_strerror_printf("%s: Internal sanity check failed, type %u", __FUNCTION__, (unsigned) dv->type);
		return -1;

	case 4:
		out[0] = 0;	/* attr must be 24-bit */
		out[1] = (da->attr >> 16) & 0xff;
		out[2] = (da->attr >> 8) & 0xff;
		out[3] = da->attr & 0xff;
		break;

	case 2:
		out[0] = (da->attr >> 8) & 0xff;
		out[1] = da->attr & 0xff;
		break;

	case 1:
		out[0] = da->attr & 0xff;
		break;
	}

	switch (dv->length) {
	default:
		fr_strerror_printf("%s: Internal sanity check failed, length %u", __FUNCTION__, (unsigned) dv->length);
		return -1;

	case 0:
		break;

	case 2:
		out[dv->type] = 0;
		out[dv->type + 1] = dv->type + 2;
		break;

	case 1:
		out[dv->type] = dv->type + 1;
		break;

	}

	if (outlen > ((unsigned) 255 - (dv->type + dv->length))) outlen = 255 - (dv->type + dv->length);

	/*
	 *	Because we've now encoded the attribute header,
	 *	if this is a TLV, we must process it via the
	 *	internal tlv function, else we get a double TLV header.
	 */
	if (tlv_stack[depth]->type == PW_TYPE_TLV) {
		len = encode_tlv_hdr_internal(out + dv->type + dv->length, outlen, packet,
					      original, secret, tlv_stack, depth, pvp);
	} else {
		len = encode_value(out + dv->type + dv->length, outlen, packet,
				   original, secret, tlv_stack, depth, pvp);
	}

	if (len <= 0) return len;

	if (dv->length) out[dv->type + dv->length - 1] += len;

#ifndef NDEBUG
	if ((fr_debug_lvl > 3) && fr_log_fp) {
		switch (dv->type) {
		default:
			break;

		case 4:
			if ((fr_debug_lvl > 3) && fr_log_fp)
				fprintf(fr_log_fp, "\t\t%02x%02x%02x%02x ", out[0], out[1], out[2], out[3]);
			break;

		case 2:
			if ((fr_debug_lvl > 3) && fr_log_fp) fprintf(fr_log_fp, "\t\t%02x%02x ", out[0], out[1]);
			break;

		case 1:
			if ((fr_debug_lvl > 3) && fr_log_fp) fprintf(fr_log_fp, "\t\t%02x ", out[0]);
			break;
		}

		switch (dv->length) {
		default:
			break;

		case 0:
			fprintf(fr_log_fp, "  ");
			break;

		case 1:
			fprintf(fr_log_fp, "%02x  ", out[dv->type]);
			break;

		case 2:
			fprintf(fr_log_fp, "%02x%02x  ", out[dv->type], out[dv->type] + 1);
			break;
		}

		FR_PROTO_HEX_DUMP("Done RFC header", out + dv->type + dv->length, len);
	}
#endif

	return dv->type + dv->length + len;
}

/** Encode a WiMAX attribute
 *
 */
static int encode_wimax_hdr(uint8_t *out, size_t outlen,
			    RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			    char const *secret,
			    fr_dict_attr_t const **tlv_stack, unsigned int depth,
			    VALUE_PAIR const **pvp)
{
	int			len;
	uint32_t		lvalue;
	int			hdr_len;
	uint8_t			*start = out;
	VALUE_PAIR const	*vp = *pvp;

	VERIFY_VP(vp);
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	/*
	 *	Double-check for WiMAX format.
	 */
	if (!vp->da->flags.wimax) {
		fr_strerror_printf("%s: Called for non-WIMAX VSA", __FUNCTION__);
		return -1;
	}

	/*
	 *	Not enough freespace for:
	 *		attr, len, vendor-id, vsa, vsalen, continuation
	 */
	if (outlen < 9) return 0;

	if (tlv_stack[depth++]->attr != PW_VENDOR_SPECIFIC) {
		fr_strerror_printf("%s: level[1] of tlv_stack is incorrect, must be Vendor-Specific (26)",
				   __FUNCTION__);
		return -1;
	}
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	if (tlv_stack[depth++]->attr != VENDORPEC_WIMAX) {
		fr_strerror_printf("%s: level[2] of tlv_stack is incorrect, must be Wimax vendor %i", __FUNCTION__,
				   VENDORPEC_WIMAX);
		return -1;
	}
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	/*
	 *	Build the Vendor-Specific header
	 */
	out = start;
	out[0] = PW_VENDOR_SPECIFIC;
	out[1] = 9;
	lvalue = htonl(vp->da->vendor);
	memcpy(out + 2, &lvalue, 4);

	/*
	 *	Encode the first attribute
	 */
	out[6] = tlv_stack[depth]->attr;
	out[7] = 3;
	out[8] = 0;		/* continuation byte */
	hdr_len = 9;

	if (tlv_stack[depth]->type == PW_TYPE_TLV) {
		len = encode_tlv_hdr_internal(out + out[1], outlen - hdr_len, packet,
					      original, secret, tlv_stack, depth, pvp);
		if (len <= 0) return len;
	} else {
		len = encode_value(out + out[1], outlen - hdr_len, packet, original, secret, tlv_stack, depth, pvp);
		if (len <= 0) return len;
	}

	/*
	 *	There may be more than 252 octets of data encoded in
	 *	the attribute.  If so, move the data up in the packet,
	 *	and copy the existing header over.  Set the "C" flag
	 *	ONLY after copying the rest of the data.
	 */
	if (len > (255 - out[1])) {
		return attr_shift(start, start + outlen, out, hdr_len, len, 8, 7);
	}

	out[1] += len;
	out[7] += len;

#ifndef NDEBUG
	if ((fr_debug_lvl > 3) && fr_log_fp) {
		fprintf(fr_log_fp, "\t\t%02x %02x  %02x%02x%02x%02x (%u)  %02x %02x %02x   ",
			out[0], out[1],
			out[2], out[3], out[4], out[5],
			(out[3] << 16) | (out[4] << 8) | out[5],
			out[6], out[7], out[8]);
		FR_PROTO_HEX_DUMP("Done wimax header", out + 9, len);
	}
#endif

	return (out + out[1]) - start;
}

/** Encode a Vendor-Specific attribute
 *
 */
static int encode_vsa_hdr(uint8_t *out, size_t outlen,
			  RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			  char const *secret,
			  fr_dict_attr_t const **tlv_stack, unsigned int depth,
			  VALUE_PAIR const **pvp)
{
	ssize_t			len;
	uint32_t		lvalue;
	fr_dict_attr_t const	*da = tlv_stack[depth];

	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	if (da->type != PW_TYPE_VSA) {
		fr_strerror_printf("%s: Expected type \"vsa\" got \"%s\"", __FUNCTION__,
				   fr_int2str(dict_attr_types, da->type, "?Unknown?"));
		return -1;
	}

	/*
	 *	Double-check for WiMAX format.
	 */
	if (da->flags.wimax) return encode_wimax_hdr(out, outlen, packet, original, secret, tlv_stack, depth + 1, pvp);

	/*
	 *	Not enough freespace for: attr, len, vendor-id
	 */
	if (outlen < 6) return 0;

	/*
	 *	Build the Vendor-Specific header
	 */
	out[0] = PW_VENDOR_SPECIFIC;
	out[1] = 6;

	/*
	 *	Now process the vendor ID part (which is one attribute deeper)
	 */
	da = tlv_stack[++depth];
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	if (da->type != PW_TYPE_VENDOR) {
		fr_strerror_printf("%s: Expected type \"vsa\" got \"%s\"", __FUNCTION__,
				   fr_int2str(dict_attr_types, da->type, "?Unknown?"));
		return -1;
	}

	lvalue = htonl(da->attr);
	memcpy(out + 2, &lvalue, 4);	/* Copy in the 32bit vendor ID */

	if (outlen > ((unsigned) 255 - out[1])) outlen = 255 - out[1];

	len = encode_vendor_attr_hdr(out + out[1], outlen, packet, original, secret, tlv_stack, depth + 1, pvp);
	if (len < 0) return len;

#ifndef NDEBUG
	if ((fr_debug_lvl > 3) && fr_log_fp) {
		fprintf(fr_log_fp, "\t\t%02x %02x  %02x%02x%02x%02x (%u)  ",
			out[0], out[1],
			out[2], out[3], out[4], out[5],
			(out[3] << 16) | (out[4] << 8) | out[5]);
		FR_PROTO_HEX_DUMP("Done VSA header", out + 6, len);
	}
#endif
	out[1] += len;

	return out[1];
}

/** Encode an RFC standard attribute 1..255
 *
 */
static int encode_rfc_hdr(uint8_t *out, size_t outlen,
			  RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			  char const *secret,
			  fr_dict_attr_t const **tlv_stack, unsigned int depth,
			  VALUE_PAIR const **pvp)
{
	VALUE_PAIR const *vp = *pvp;

	/*
	 *	Sanity checks
	 */
	VERIFY_VP(vp);
	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	switch (tlv_stack[depth]->type) {
	case PW_TYPE_STRUCTURAL:
		fr_strerror_printf("%s: Expected leaf type got \"%s\"", __FUNCTION__,
				   fr_int2str(dict_attr_types, tlv_stack[depth]->type, "?Unknown?"));
		return -1;

	default:
		if ((vp->da->attr == 0) || (vp->da->attr > 255)) {
			fr_strerror_printf("%s: Called with non-standard attribute %u", __FUNCTION__, vp->da->attr);
			return -1;
		}
		break;
	}

	/*
	 *	Only CUI is allowed to have zero length.
	 *	Thank you, WiMAX!
	 */
	if ((vp->vp_length == 0) &&  (vp->da->attr == PW_CHARGEABLE_USER_IDENTITY)) {
		out[0] = PW_CHARGEABLE_USER_IDENTITY;
		out[1] = 2;

		*pvp = vp->next;
		fr_proto_tlv_stack_build(tlv_stack, *pvp ? (*pvp)->da : NULL);
		return 2;
	}

	/*
	 *	Message-Authenticator is hard-coded.
	 */
	if (!vp->da->vendor && (vp->da->attr == PW_MESSAGE_AUTHENTICATOR)) {
		if (outlen < 18) return -1;

		out[0] = PW_MESSAGE_AUTHENTICATOR;
		out[1] = 18;
		memset(out + 2, 0, 16);
#ifndef NDEBUG
		if ((fr_debug_lvl > 3) && fr_log_fp) {
			fprintf(fr_log_fp, "\t\t50 12 ...\n");
		}
#endif
		*pvp = (*pvp)->next;
		fr_proto_tlv_stack_build(tlv_stack, *pvp ? (*pvp)->da : NULL);
		return 18;
	}

	return encode_rfc_hdr_internal(out, outlen, packet, original, secret, tlv_stack, depth, pvp);
}

/** Encode a data structure into a RADIUS attribute
 *
 * This is the main entry point into the encoder.  It sets up the encoder array
 * we use for tracking our TLV/VSA/EVS nesting and then calls the appropriate
 * dispatch function.
 */
int fr_radius_encode_pair(uint8_t *out, size_t outlen,
			  RADIUS_PACKET const *packet, RADIUS_PACKET const *original,
			  char const *secret, VALUE_PAIR const **pvp)
{
	VALUE_PAIR const *vp;
	int ret;
	size_t attr_len;

	fr_dict_attr_t const *tlv_stack[MAX_TLV_STACK + 1];
	fr_dict_attr_t const *da = NULL;

	if (!pvp || !*pvp || !out || (outlen <= 2)) return -1;
	vp = *pvp;

	VERIFY_VP(vp);

	if (vp->da->depth > MAX_TLV_STACK) {
		fr_strerror_printf("%s: Attribute depth %i exceeds maximum nesting depth %i",
				   __FUNCTION__, vp->da->depth, MAX_TLV_STACK);
		return -1;
	}

	fr_proto_tlv_stack_build(tlv_stack, vp->da);
	FR_PROTO_STACK_PRINT(tlv_stack, 0);

	/*
	 *	Nested structures of attributes can't be longer than
	 *	255 bytes, so each call to an encode function can
	 *	only use 255 bytes of buffer space at a time.
	 */
	attr_len = (outlen > UINT8_MAX) ? UINT8_MAX : outlen;

	da = tlv_stack[0];
	switch (da->type) {
	default:
		/*
		 *	Ignore non-protocol attributes.
		 */
		if (da->attr > 255) return 0;

		if (!da->flags.concat) {
			ret = encode_rfc_hdr(out, attr_len, packet, original, secret, tlv_stack, 0, pvp);

		} else {
			/*
			 *	Attributes like EAP-Message are marked as
			 *	"concat", which means that they are fragmented
			 *	using a different scheme than the "long
			 *	extended" one.
			 */
			ret = encode_concat(out, outlen, packet, original, secret, tlv_stack, 0, pvp);
		}
		break;

	case PW_TYPE_VSA:
		if (!(*pvp)->da->flags.wimax) {
			ret = encode_vsa_hdr(out, attr_len, packet, original, secret, tlv_stack, 0, pvp);

		} else {			 
			/*
			 *	WiMAX has a non-standard format for
			 *	its VSAs.  And, it can do "long"
			 *	attributes by fragmenting them inside
			 *	of the WiMAX VSA space.
			 */
			ret = encode_wimax_hdr(out, outlen, packet, original, secret, tlv_stack, 0, pvp);
		}
		break;

	case PW_TYPE_TLV:
		ret = encode_tlv_hdr(out, attr_len, packet, original, secret, tlv_stack, 0, pvp);
		break;

	case PW_TYPE_EXTENDED:
		ret = encode_extended_hdr(out, attr_len, packet, original, secret, tlv_stack, 0, pvp);
		break;

	case PW_TYPE_LONG_EXTENDED:
		/*
		 *	These attributes can be longer than 253
		 *	octets.  We therfore fragment the data across
		 *	multiple attributes.
		 */
		ret = encode_extended_hdr(out, outlen, packet, original, secret, tlv_stack, 0, pvp);
		break;

	case PW_TYPE_EVS:
		fr_strerror_printf("%s: Top level \"evs\" attribute is invalid", __FUNCTION__);
		return -1;
	}

	if (ret < 0) return ret;

	/*
	 *	We couldn't do it, so we didn't do anything.
	 */
	if (*pvp == vp) {
		fr_strerror_printf("%s: Nested attribute structure too large to encode", __FUNCTION__);
		return -1;
	}

	return ret;
}
