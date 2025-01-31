// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Backend for the LRNG providing the cryptographic primitives using the
 * kernel crypto API.
 *
 * Copyright (C) 2018 - 2019, Stephan Mueller <smueller@chronox.de>
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <crypto/hash.h>
#include <crypto/rng.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/lrng.h>

static char *drng_name = NULL;
module_param(drng_name, charp, 0444);
MODULE_PARM_DESC(drng_name, "Kernel crypto API name of DRNG");

static char *pool_hash = "sha512";
module_param(pool_hash, charp, 0444);
MODULE_PARM_DESC(pool_hash,
		 "Kernel crypto API name of hash or keyed message digest to read the entropy pool");

static char *seed_hash = NULL;
module_param(seed_hash, charp, 0444);
MODULE_PARM_DESC(seed_hash,
		 "Kernel crypto API name of hash with output size equal to seedsize of DRNG to bring seed string to the size required by the DRNG");

struct lrng_hash_info {
	struct shash_desc shash;
	char ctx[];
};

struct lrng_drng_info {
	struct crypto_rng *kcapi_rng;
	struct lrng_hash_info *lrng_hash;
};

static struct lrng_hash_info *_lrng_kcapi_hash_alloc(const char *name)
{
	struct lrng_hash_info *lrng_hash;
	struct crypto_shash *tfm;
	int size;

	if (!name) {
		pr_err("Hash name missing\n");
		return ERR_PTR(-EINVAL);
	}

	tfm = crypto_alloc_shash(name, 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("could not allocate hash %s\n", name);
		return ERR_CAST(tfm);
	}

	size = sizeof(struct lrng_hash_info) + crypto_shash_descsize(tfm);
	lrng_hash = kmalloc(size, GFP_KERNEL);
	if (!lrng_hash) {
		crypto_free_shash(tfm);
		return ERR_PTR(-ENOMEM);
	}

	lrng_hash->shash.tfm = tfm;

	return lrng_hash;
}

static inline u32 _lrng_kcapi_hash_digestsize(struct lrng_hash_info *lrng_hash)
{
	struct shash_desc *shash = &lrng_hash->shash;
	struct crypto_shash *tfm = shash->tfm;

	return crypto_shash_digestsize(tfm);
}

static inline void _lrng_kcapi_hash_free(struct lrng_hash_info *lrng_hash)
{
	struct shash_desc *shash = &lrng_hash->shash;
	struct crypto_shash *tfm = shash->tfm;

	crypto_free_shash(tfm);
	kfree(lrng_hash);
}

static void *lrng_kcapi_hash_alloc(const u8 *key, u32 keylen)
{
	struct lrng_hash_info *lrng_hash;
	int ret;

	lrng_hash = _lrng_kcapi_hash_alloc(pool_hash);
	if (IS_ERR(lrng_hash))
		return ERR_CAST(lrng_hash);

	/* If the used hash is no MAC, ignore the ENOSYS return code */
	ret = crypto_shash_setkey(lrng_hash->shash.tfm, key, keylen);
	if (ret && ret != -ENOSYS) {
		pr_err("could not set the key for MAC\n");
		_lrng_kcapi_hash_free(lrng_hash);
		return ERR_PTR(ret);
	}

	pr_info("Hash %s allocated\n", pool_hash);

	return lrng_hash;
}

static void lrng_kcapi_hash_dealloc(void *hash)
{
	struct lrng_hash_info *lrng_hash = (struct lrng_hash_info *)hash;

	_lrng_kcapi_hash_free(lrng_hash);
	pr_info("Hash %s deallocated\n", pool_hash);
}

static u32 lrng_kcapi_hash_digestsize(void *hash)
{
	struct lrng_hash_info *lrng_hash = (struct lrng_hash_info *)hash;

	return _lrng_kcapi_hash_digestsize(lrng_hash);
}

static int lrng_kcapi_hash_buffer(void *hash, const u8 *inbuf, u32 inbuflen,
				  u8 *digest)
{
	struct lrng_hash_info *lrng_hash = (struct lrng_hash_info *)hash;
	struct shash_desc *shash = &lrng_hash->shash;

	return crypto_shash_digest(shash, inbuf, inbuflen, digest);
}

static int lrng_kcapi_drng_seed_helper(void *drng, const u8 *inbuf,
				       u32 inbuflen)
{
	struct lrng_drng_info *lrng_drng_info = (struct lrng_drng_info *)drng;
	struct crypto_rng *kcapi_rng = lrng_drng_info->kcapi_rng;
	struct lrng_hash_info *lrng_hash = lrng_drng_info->lrng_hash;

	if (lrng_hash) {
		struct shash_desc *shash = &lrng_hash->shash;
		u32 digestsize = _lrng_kcapi_hash_digestsize(lrng_hash);
		u8 digest[64] __aligned(8);
		int ret;

		BUG_ON(digestsize > 64);

		ret = crypto_shash_digest(shash, inbuf, inbuflen, digest);
		if (ret)
			return ret;

		ret = crypto_rng_reset(kcapi_rng, digest, digestsize);
		if (ret)
			return ret;

		memzero_explicit(digest, digestsize);

		return 0;
	} else {
		return crypto_rng_reset(kcapi_rng, inbuf, inbuflen);
	}
}

static int lrng_kcapi_drng_generate_helper(void *drng, u8 *outbuf,
					   u32 outbuflen)
{
	struct lrng_drng_info *lrng_drng_info = (struct lrng_drng_info *)drng;
	struct crypto_rng *kcapi_rng = lrng_drng_info->kcapi_rng;
	int ret = crypto_rng_get_bytes(kcapi_rng, outbuf, outbuflen);

	if (ret < 0)
		return ret;

	return outbuflen;
}

static void *lrng_kcapi_drng_alloc(u32 sec_strength)
{
	struct lrng_drng_info *lrng_drng_info;
	struct crypto_rng *kcapi_rng;
	int seedsize;
	void *ret =  ERR_PTR(-ENOMEM);

	if (!drng_name) {
		pr_err("DRNG name missing\n");
		return ERR_PTR(-EINVAL);
	}

	if (!memcmp(drng_name, "drbg", 4)) {
		pr_err("SP800-90A DRBG cannot be allocated using lrng_kcapi "
		       "backend, use lrng_drbg backend instead\n");
		return ERR_PTR(-EINVAL);
	}

	if (!memcmp(drng_name, "stdrng", 6)) {
		pr_err("stdrng cannot be allocated using lrng_kcapi backend, "
		       "it is too unspecific and potentially may allocate the"
		       "DRBG\n");
		return ERR_PTR(-EINVAL);
	}

	lrng_drng_info = kmalloc(sizeof(*lrng_drng_info), GFP_KERNEL);
	if (!lrng_drng_info)
		return ERR_PTR(-ENOMEM);

	kcapi_rng = crypto_alloc_rng(drng_name, 0, 0);
	if (IS_ERR(kcapi_rng)) {
		pr_err("DRNG %s cannot be allocated\n", drng_name);
		ret = ERR_CAST(kcapi_rng);
		goto free;
	}
	lrng_drng_info->kcapi_rng = kcapi_rng;

	seedsize =  crypto_rng_seedsize(kcapi_rng);

	if (sec_strength > seedsize)
		pr_info("Seedsize DRNG (%u bits) lower than "
			"security strength of LRNG noise source (%u bits)\n",
			crypto_rng_seedsize(kcapi_rng) * 8,
			sec_strength * 8);

	if (seedsize) {
		struct lrng_hash_info *lrng_hash;

		if (!seed_hash) {
			switch (seedsize) {
			case 32:
				seed_hash = "sha256";
				break;
			case 48:
				seed_hash = "sha384";
				break;
			case 64:
				seed_hash = "sha512";
				break;
			default:
				pr_err("Seed size %d cannot be processed\n",
				       seedsize);
				goto dealloc;
				break;
			}
		}

		lrng_hash = _lrng_kcapi_hash_alloc(seed_hash);
		if (IS_ERR(lrng_hash)) {
			ret = ERR_CAST(lrng_hash);
			goto dealloc;
		}

		if (seedsize != _lrng_kcapi_hash_digestsize(lrng_hash)) {
			pr_err("Seed hash output size not equal to DRNG seed "
			       "size\n");
			_lrng_kcapi_hash_free(lrng_hash);
			ret = ERR_PTR(-EINVAL);
			goto dealloc;
		}

		lrng_drng_info->lrng_hash = lrng_hash;

		pr_info("Seed hash %s allocated\n", seed_hash);
	} else {
		lrng_drng_info->lrng_hash = NULL;
	}

	pr_info("Kernel crypto API DRNG %s allocated\n", drng_name);

	return lrng_drng_info;

dealloc:
	crypto_free_rng(kcapi_rng);
free:
	kfree(lrng_drng_info);
	return ret;
}

static void lrng_kcapi_drng_dealloc(void *drng)
{
	struct lrng_drng_info *lrng_drng_info = (struct lrng_drng_info *)drng;
	struct crypto_rng *kcapi_rng = lrng_drng_info->kcapi_rng;
	struct lrng_hash_info *lrng_hash = lrng_drng_info->lrng_hash;

	crypto_free_rng(kcapi_rng);
	if (lrng_hash) {
		_lrng_kcapi_hash_free(lrng_hash);
		pr_info("Seed hash %s deallocated\n", seed_hash);
	}
	kfree(lrng_drng_info);
	pr_info("DRNG %s deallocated\n", drng_name);
}

static const char *lrng_kcapi_drng_name(void)
{
	return drng_name;
}

static const char *lrng_kcapi_pool_hash(void)
{
	return pool_hash;
}

const static struct lrng_crypto_cb lrng_kcapi_crypto_cb = {
	.lrng_drng_name			= lrng_kcapi_drng_name,
	.lrng_hash_name			= lrng_kcapi_pool_hash,
	.lrng_drng_alloc		= lrng_kcapi_drng_alloc,
	.lrng_drng_dealloc		= lrng_kcapi_drng_dealloc,
	.lrng_drng_seed_helper		= lrng_kcapi_drng_seed_helper,
	.lrng_drng_generate_helper	= lrng_kcapi_drng_generate_helper,
	.lrng_drng_generate_helper_full	= lrng_kcapi_drng_generate_helper,
	.lrng_hash_alloc		= lrng_kcapi_hash_alloc,
	.lrng_hash_dealloc		= lrng_kcapi_hash_dealloc,
	.lrng_hash_digestsize		= lrng_kcapi_hash_digestsize,
	.lrng_hash_buffer		= lrng_kcapi_hash_buffer,
};

static int __init lrng_kcapi_init(void)
{
	return lrng_set_drng_cb(&lrng_kcapi_crypto_cb);
}
static void __exit lrng_kcapi_exit(void)
{
	lrng_set_drng_cb(NULL);
}

late_initcall(lrng_kcapi_init);
module_exit(lrng_kcapi_exit);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Linux Random Number Generator - kernel crypto API DRNG backend");
