// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Laboratory instrument for the Anyka encrypt engine: throughput and
 * known-answer comparison for any skcipher the crypto API can resolve.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#include <crypto/skcipher.h>

#define AKB_RESULTS_SIZE	16384
#define AKB_CMD_SIZE		256
#define AKB_MAX_SIZE		(1U << 21)
#define AKB_KEY_SIZE		16

enum akb_shape {
	AKB_SHAPE_CONTIG,	/* one physically contiguous buffer */
	AKB_SHAPE_SG,		/* a chain of order-0 pages */
};

struct akb_buf {
	struct scatterlist	*sgl;
	unsigned int		nents;
	struct page		**pages;
	void			*contig;
	unsigned int		len;
};

struct akb_wait {
	struct completion	done;
	int			err;
};

static struct dentry *akb_dir;
static DEFINE_MUTEX(akb_lock);
static char *akb_results;
static size_t akb_results_len;

static const u8 akb_key[AKB_KEY_SIZE] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
};

static __printf(1, 2) void akb_emit(const char *fmt, ...)
{
	va_list args;
	int n;

	if (akb_results_len >= AKB_RESULTS_SIZE - 1)
		return;

	va_start(args, fmt);
	n = vsnprintf(akb_results + akb_results_len,
		      AKB_RESULTS_SIZE - akb_results_len, fmt, args);
	va_end(args);

	if (n > 0)
		akb_results_len += min_t(size_t, (size_t)n,
					 AKB_RESULTS_SIZE - 1 - akb_results_len);
}

static void akb_complete(struct crypto_async_request *req, int err)
{
	struct akb_wait *w = req->data;

	if (err == -EINPROGRESS)
		return;
	w->err = err;
	complete(&w->done);
}

static int akb_wait(struct akb_wait *w, int ret)
{
	switch (ret) {
	case 0:
		return 0;
	case -EINPROGRESS:
	case -EBUSY:
		wait_for_completion(&w->done);
		reinit_completion(&w->done);
		return w->err;
	default:
		return ret;
	}
}

static void akb_buf_free(struct akb_buf *b)
{
	unsigned int i;

	if (b->pages) {
		for (i = 0; i < b->nents; i++)
			if (b->pages[i])
				__free_page(b->pages[i]);
		kfree(b->pages);
	}
	kfree(b->contig);
	kfree(b->sgl);
	memset(b, 0, sizeof(*b));
}

/*
 * A contiguous buffer above order-2 is not obtainable on this board once it
 * has been up for a few minutes, so the caller is told which shape it got
 * rather than the allocation being retried at a smaller order.
 */
static int akb_buf_alloc(struct akb_buf *b, unsigned int len,
			 enum akb_shape shape)
{
	unsigned int i;

	memset(b, 0, sizeof(*b));
	b->len = len;

	if (shape == AKB_SHAPE_CONTIG) {
		b->nents = 1;
		b->contig = kmalloc(len, GFP_KERNEL);
		if (!b->contig)
			return -ENOMEM;
		b->sgl = kmalloc(sizeof(*b->sgl), GFP_KERNEL);
		if (!b->sgl)
			goto err;
		sg_init_one(b->sgl, b->contig, len);
		get_random_bytes(b->contig, len);
		return 0;
	}

	b->nents = DIV_ROUND_UP(len, PAGE_SIZE);
	b->pages = kcalloc(b->nents, sizeof(*b->pages), GFP_KERNEL);
	if (!b->pages)
		return -ENOMEM;
	b->sgl = kcalloc(b->nents, sizeof(*b->sgl), GFP_KERNEL);
	if (!b->sgl)
		goto err;
	sg_init_table(b->sgl, b->nents);

	for (i = 0; i < b->nents; i++) {
		unsigned int this = min_t(unsigned int, PAGE_SIZE,
					  len - i * PAGE_SIZE);

		b->pages[i] = alloc_page(GFP_KERNEL);
		if (!b->pages[i])
			goto err;
		get_random_bytes(page_address(b->pages[i]), this);
		sg_set_page(&b->sgl[i], b->pages[i], this, 0);
	}
	return 0;
err:
	akb_buf_free(b);
	return -ENOMEM;
}

static void akb_buf_read(struct akb_buf *b, void *out)
{
	unsigned int i, off = 0;

	if (b->contig) {
		memcpy(out, b->contig, b->len);
		return;
	}
	for (i = 0; i < b->nents; i++) {
		memcpy((u8 *)out + off, page_address(b->pages[i]),
		       b->sgl[i].length);
		off += b->sgl[i].length;
	}
}

static int akb_run_one(const char *alg, bool enc, enum akb_shape shape,
		       unsigned int size, unsigned int msec)
{
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
	struct akb_buf src, dst;
	struct akb_wait wait;
	u8 iv[16];
	u64 t0, t1, elapsed;
	u64 iters = 0, bytes;
	unsigned int ivsize;
	int err;

	tfm = crypto_alloc_skcipher(alg, 0, 0);
	if (IS_ERR(tfm)) {
		akb_emit("error alg=%s alloc=%ld\n", alg, PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}

	if (size % crypto_skcipher_blocksize(tfm)) {
		akb_emit("error alg=%s size=%u not-a-block-multiple\n",
			 alg, size);
		err = -EINVAL;
		goto out_tfm;
	}

	err = crypto_skcipher_setkey(tfm, akb_key, AKB_KEY_SIZE);
	if (err) {
		akb_emit("error alg=%s setkey=%d\n", alg, err);
		goto out_tfm;
	}

	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto out_tfm;
	}

	err = akb_buf_alloc(&src, size, shape);
	if (err) {
		akb_emit("error alg=%s size=%u shape=%s src-alloc=%d\n", alg,
			 size, shape == AKB_SHAPE_CONTIG ? "contig" : "sg",
			 err);
		goto out_req;
	}
	err = akb_buf_alloc(&dst, size, shape);
	if (err) {
		akb_emit("error alg=%s size=%u shape=%s dst-alloc=%d\n", alg,
			 size, shape == AKB_SHAPE_CONTIG ? "contig" : "sg",
			 err);
		goto out_src;
	}

	init_completion(&wait.done);
	wait.err = 0;
	skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      akb_complete, &wait);

	ivsize = crypto_skcipher_ivsize(tfm);
	memset(iv, 0x5a, sizeof(iv));
	skcipher_request_set_crypt(req, src.sgl, dst.sgl, size,
				   ivsize ? iv : NULL);

	/* One untimed pass so module autoload and first-touch faults are out. */
	err = akb_wait(&wait, enc ? crypto_skcipher_encrypt(req) :
				    crypto_skcipher_decrypt(req));
	if (err) {
		akb_emit("error alg=%s size=%u first-op=%d\n", alg, size, err);
		goto out_dst;
	}

	t0 = ktime_get_ns();
	do {
		memset(iv, 0x5a, sizeof(iv));
		skcipher_request_set_crypt(req, src.sgl, dst.sgl, size,
					   ivsize ? iv : NULL);
		err = akb_wait(&wait, enc ? crypto_skcipher_encrypt(req) :
					    crypto_skcipher_decrypt(req));
		if (err) {
			akb_emit("error alg=%s size=%u op=%d\n", alg, size, err);
			goto out_dst;
		}
		iters++;
		t1 = ktime_get_ns();
	} while (t1 - t0 < (u64)msec * NSEC_PER_MSEC);

	elapsed = t1 - t0;
	bytes = iters * size;

	akb_emit("alg=%s dir=%s shape=%s size=%u iters=%llu ns=%llu "
		 "kBps=%llu nsper=%llu\n",
		 alg, enc ? "enc" : "dec",
		 shape == AKB_SHAPE_CONTIG ? "contig" : "sg", size,
		 iters, elapsed,
		 div64_u64(bytes * NSEC_PER_SEC, elapsed * 1024),
		 div64_u64(elapsed, iters));
	err = 0;

out_dst:
	akb_buf_free(&dst);
out_src:
	akb_buf_free(&src);
out_req:
	skcipher_request_free(req);
out_tfm:
	crypto_free_skcipher(tfm);
	return err;
}

/*
 * Known-answer by comparison: the driver under test must agree with whatever
 * the crypto API resolves for the generic name, over both directions, both
 * buffer shapes and a spread of lengths including ones that straddle a page.
 */
static int akb_kat_one(const char *alg, const char *ref, enum akb_shape shape,
		       unsigned int size)
{
	struct crypto_skcipher *tfm = NULL, *rtfm = NULL;
	struct skcipher_request *req = NULL, *rreq = NULL;
	struct akb_buf src, dst, rdst;
	struct akb_wait wait, rwait;
	u8 iv[16], riv[16];
	void *a = NULL, *b = NULL;
	unsigned int ivsize;
	int err, dir;
	bool have_src = false, have_dst = false, have_rdst = false;

	tfm = crypto_alloc_skcipher(alg, 0, 0);
	if (IS_ERR(tfm)) {
		akb_emit("kat alg=%s alloc=%ld FAIL\n", alg, PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}
	rtfm = crypto_alloc_skcipher(ref, 0, 0);
	if (IS_ERR(rtfm)) {
		akb_emit("kat ref=%s alloc=%ld FAIL\n", ref, PTR_ERR(rtfm));
		err = PTR_ERR(rtfm);
		rtfm = NULL;
		goto out;
	}

	err = crypto_skcipher_setkey(tfm, akb_key, AKB_KEY_SIZE);
	if (!err)
		err = crypto_skcipher_setkey(rtfm, akb_key, AKB_KEY_SIZE);
	if (err) {
		akb_emit("kat alg=%s setkey=%d FAIL\n", alg, err);
		goto out;
	}

	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	rreq = skcipher_request_alloc(rtfm, GFP_KERNEL);
	if (!req || !rreq) {
		err = -ENOMEM;
		goto out;
	}

	err = akb_buf_alloc(&src, size, shape);
	if (err)
		goto out;
	have_src = true;
	err = akb_buf_alloc(&dst, size, shape);
	if (err)
		goto out;
	have_dst = true;
	err = akb_buf_alloc(&rdst, size, shape);
	if (err)
		goto out;
	have_rdst = true;

	a = kmalloc(size, GFP_KERNEL);
	b = kmalloc(size, GFP_KERNEL);
	if (!a || !b) {
		err = -ENOMEM;
		goto out;
	}

	init_completion(&wait.done);
	init_completion(&rwait.done);
	wait.err = 0;
	rwait.err = 0;
	skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      akb_complete, &wait);
	skcipher_request_set_callback(rreq, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      akb_complete, &rwait);
	ivsize = crypto_skcipher_ivsize(tfm);

	for (dir = 0; dir < 2; dir++) {
		memset(iv, 0x5a, sizeof(iv));
		memset(riv, 0x5a, sizeof(riv));
		skcipher_request_set_crypt(req, src.sgl, dst.sgl, size,
					   ivsize ? iv : NULL);
		skcipher_request_set_crypt(rreq, src.sgl, rdst.sgl, size,
					   ivsize ? riv : NULL);

		err = akb_wait(&wait, dir ? crypto_skcipher_decrypt(req) :
					    crypto_skcipher_encrypt(req));
		if (err) {
			akb_emit("kat alg=%s size=%u dir=%s op=%d FAIL\n",
				 alg, size, dir ? "dec" : "enc", err);
			goto out;
		}
		err = akb_wait(&rwait, dir ? crypto_skcipher_decrypt(rreq) :
					     crypto_skcipher_encrypt(rreq));
		if (err)
			goto out;

		akb_buf_read(&dst, a);
		akb_buf_read(&rdst, b);
		if (memcmp(a, b, size)) {
			akb_emit("kat alg=%s ref=%s shape=%s size=%u dir=%s "
				 "MISMATCH\n", alg, ref,
				 shape == AKB_SHAPE_CONTIG ? "contig" : "sg",
				 size, dir ? "dec" : "enc");
			err = -EBADMSG;
			goto out;
		}
		/* A caller chains a stream by reusing the returned IV, so an
		 * implementation that leaves it alone is wrong in a way the
		 * ciphertext of a single request cannot show.
		 */
		if (ivsize && memcmp(iv, riv, ivsize)) {
			akb_emit("kat alg=%s ref=%s shape=%s size=%u dir=%s "
				 "IV-MISMATCH\n", alg, ref,
				 shape == AKB_SHAPE_CONTIG ? "contig" : "sg",
				 size, dir ? "dec" : "enc");
			err = -EBADMSG;
			goto out;
		}
	}
	err = 0;
out:
	kfree(b);
	kfree(a);
	if (have_rdst)
		akb_buf_free(&rdst);
	if (have_dst)
		akb_buf_free(&dst);
	if (have_src)
		akb_buf_free(&src);
	skcipher_request_free(rreq);
	skcipher_request_free(req);
	if (!IS_ERR_OR_NULL(rtfm))
		crypto_free_skcipher(rtfm);
	if (!IS_ERR_OR_NULL(tfm))
		crypto_free_skcipher(tfm);
	return err;
}

static const unsigned int akb_kat_sizes[] = {
	16, 32, 48, 64, 240, 256, 496, 512, 1008, 1024,
	4080, 4096, 4112, 8192, 16368, 16384,
};

static int akb_kat(const char *alg, const char *ref)
{
	unsigned int i;
	int err, fails = 0;

	for (i = 0; i < ARRAY_SIZE(akb_kat_sizes); i++) {
		err = akb_kat_one(alg, ref, AKB_SHAPE_CONTIG,
				  akb_kat_sizes[i]);
		if (err)
			fails++;
		err = akb_kat_one(alg, ref, AKB_SHAPE_SG, akb_kat_sizes[i]);
		if (err)
			fails++;
	}
	akb_emit("kat alg=%s ref=%s points=%u fails=%d %s\n", alg, ref,
		 (unsigned int)ARRAY_SIZE(akb_kat_sizes) * 2, fails,
		 fails ? "FAIL" : "PASS");
	return fails ? -EBADMSG : 0;
}

/* NIST SP 800-38A F.1.1 and F.2.1, AES-128, under akb_key. */
static const u8 akb_nist_plain[64] = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
	0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
	0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
	0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
	0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
	0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
	0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
};

static const u8 akb_nist_ecb[64] = {
	0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60,
	0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97,
	0xf5, 0xd3, 0xd5, 0x85, 0x03, 0xb9, 0x69, 0x9d,
	0xe7, 0x85, 0x89, 0x5a, 0x96, 0xfd, 0xba, 0xaf,
	0x43, 0xb1, 0xcd, 0x7f, 0x59, 0x8e, 0xce, 0x23,
	0x88, 0x1b, 0x00, 0xe3, 0xed, 0x03, 0x06, 0x88,
	0x7b, 0x0c, 0x78, 0x5e, 0x27, 0xe8, 0xad, 0x3f,
	0x82, 0x23, 0x20, 0x71, 0x04, 0x72, 0x5d, 0xd4,
};

static const u8 akb_nist_cbc_iv[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

static const u8 akb_nist_cbc[64] = {
	0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
	0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d,
	0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee,
	0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2,
	0x73, 0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b,
	0x71, 0x16, 0xe6, 0x9e, 0x22, 0x22, 0x95, 0x16,
	0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac, 0x09,
	0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7,
};

static int akb_nist(const char *alg, bool cbc)
{
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
	struct akb_buf src, dst;
	struct akb_wait wait;
	const u8 *want = cbc ? akb_nist_cbc : akb_nist_ecb;
	u8 iv[16], out[64];
	int err;

	tfm = crypto_alloc_skcipher(alg, 0, 0);
	if (IS_ERR(tfm)) {
		akb_emit("nist alg=%s alloc=%ld FAIL\n", alg, PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}
	err = crypto_skcipher_setkey(tfm, akb_key, AKB_KEY_SIZE);
	if (err)
		goto out_tfm;
	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto out_tfm;
	}
	err = akb_buf_alloc(&src, sizeof(akb_nist_plain), AKB_SHAPE_CONTIG);
	if (err)
		goto out_req;
	err = akb_buf_alloc(&dst, sizeof(akb_nist_plain), AKB_SHAPE_CONTIG);
	if (err)
		goto out_src;
	memcpy(src.contig, akb_nist_plain, sizeof(akb_nist_plain));

	init_completion(&wait.done);
	wait.err = 0;
	skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				      akb_complete, &wait);
	memcpy(iv, akb_nist_cbc_iv, sizeof(iv));
	skcipher_request_set_crypt(req, src.sgl, dst.sgl,
				   sizeof(akb_nist_plain), cbc ? iv : NULL);

	err = akb_wait(&wait, crypto_skcipher_encrypt(req));
	if (err) {
		akb_emit("nist alg=%s op=%d FAIL\n", alg, err);
		goto out_dst;
	}
	akb_buf_read(&dst, out);
	if (memcmp(out, want, sizeof(out))) {
		akb_emit("nist alg=%s MISMATCH\n", alg);
		err = -EBADMSG;
	} else {
		akb_emit("nist alg=%s PASS\n", alg);
	}

out_dst:
	akb_buf_free(&dst);
out_src:
	akb_buf_free(&src);
out_req:
	skcipher_request_free(req);
out_tfm:
	crypto_free_skcipher(tfm);
	return err;
}

static ssize_t akb_run_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	char cmd[AKB_CMD_SIZE];
	char alg[CRYPTO_MAX_ALG_NAME], word[16], ref[CRYPTO_MAX_ALG_NAME];
	unsigned int size, msec;
	enum akb_shape shape;
	bool enc;
	int n;

	(void)file;
	(void)ppos;

	if (count == 0 || count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';

	mutex_lock(&akb_lock);

	if (!strncmp(cmd, "clear", 5)) {
		akb_results_len = 0;
		goto done;
	}

	n = sscanf(cmd, "nist %63s %15s", alg, word);
	if (n == 2) {
		akb_nist(alg, !strcmp(word, "cbc"));
		goto done;
	}

	n = sscanf(cmd, "kat %63s %63s", alg, ref);
	if (n == 2) {
		akb_kat(alg, ref);
		goto done;
	}

	n = sscanf(cmd, "%63s %15s %15s %u %u", alg, word, ref, &size, &msec);
	if (n != 5) {
		mutex_unlock(&akb_lock);
		return -EINVAL;
	}
	enc = !strcmp(word, "enc");
	if (!enc && strcmp(word, "dec")) {
		mutex_unlock(&akb_lock);
		return -EINVAL;
	}
	if (!strcmp(ref, "contig")) {
		shape = AKB_SHAPE_CONTIG;
	} else if (!strcmp(ref, "sg")) {
		shape = AKB_SHAPE_SG;
	} else {
		mutex_unlock(&akb_lock);
		return -EINVAL;
	}
	if (size == 0 || size > AKB_MAX_SIZE || msec == 0 || msec > 60000) {
		mutex_unlock(&akb_lock);
		return -EINVAL;
	}

	akb_run_one(alg, enc, shape, size, msec);

done:
	mutex_unlock(&akb_lock);
	return count;
}

static ssize_t akb_results_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	ssize_t ret;

	(void)file;
	mutex_lock(&akb_lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, akb_results,
				      akb_results_len);
	mutex_unlock(&akb_lock);
	return ret;
}

static const struct file_operations akb_run_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= akb_run_write,
	.llseek	= noop_llseek,
};

static const struct file_operations akb_results_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= akb_results_read,
	.llseek	= default_llseek,
};

static int __init akb_init(void)
{
	akb_results = kzalloc(AKB_RESULTS_SIZE, GFP_KERNEL);
	if (!akb_results)
		return -ENOMEM;

	akb_dir = debugfs_create_dir("ak-crypto-bench", NULL);
	if (!akb_dir) {
		kfree(akb_results);
		return -ENODEV;
	}
	debugfs_create_file("run", 0200, akb_dir, NULL, &akb_run_fops);
	debugfs_create_file("results", 0400, akb_dir, NULL, &akb_results_fops);
	return 0;
}

static void __exit akb_exit(void)
{
	debugfs_remove_recursive(akb_dir);
	kfree(akb_results);
}

module_init(akb_init);
module_exit(akb_exit);

MODULE_DESCRIPTION("Throughput and cross-check instrument for skcipher algorithms");
MODULE_LICENSE("GPL");
