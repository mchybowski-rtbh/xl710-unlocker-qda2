/* xl710_unlock - clear the "qualified module only" bit in Intel X710/XL710 NVM.
 *
 * The PHY Capabilities section holds one struct per port. Word +0x08 of each
 * is "PHY Capabilities Misc0"; bit 11 is "Enable Module Qualification". Clear
 * it on every struct and the card stops rejecting unqualified optics.
 *
 * Finding those structs is the whole problem. The location moves with every
 * firmware revision, the Shadow RAM pointer that is supposed to lead there is
 * not reliable (Intel's own HOWTO says the image pointer may be corrupted),
 * and reading +0x08 from a base that is a few words off yields a word whose
 * bit 11 happens to be 0 - so the card reports "unlocked" while still
 * rejecting modules. Observed bases, all with a valid-looking pointer chain:
 *
 *     card / firmware        base     size   stride   Misc0
 *     X710      fw 5.x       0x6870   0x0b   0x0c     0x2b0c
 *     X710      fw 6.x       0x68f0   0x0c   0x0d     0x630c
 *     X710      fw 8.13      0x6940   0x0d   0x0e     0x6b0c
 *     XL710-QDA2 (0x1583)    0x67fa   0x0b   0x0c     0x0b10
 *
 * So the base is not computed, it is *recognised*: word 0 of a struct is its
 * payload size, structs are size+1 apart, and that size word therefore repeats
 * at exactly that stride. That signature holds on every dump above and is what
 * this tool locks onto. The pointer chain is only a hint, and it is validated
 * against the signature before anything is written.
 *
 * Derived from terpstra/xl710-unlocker and bibigon812/xl710-unlocker, both of
 * which hardcode the stride (0xc) and so patch the wrong words on most cards.
 */

#include <assert.h>
#include <getopt.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/sockios.h>

/* i40e NVM access, as the driver reinterprets struct ethtool_eeprom */
#define I40E_NVM_READ         0x0b
#define I40E_NVM_WRITE        0x0c
#define I40E_NVM_TRANS_SHIFT  8
#define I40E_NVM_SNT          0x1
#define I40E_NVM_LCB          0x2
#define I40E_NVM_SA           (I40E_NVM_SNT | I40E_NVM_LCB)
#define I40E_NVM_CSUM         0x8

#define SR_NVM_EETRACK_LO     0x2d   /* EETRACK id, identifies the NVM build */
#define SR_NVM_EETRACK_HI     0x2e
#define SR_EMP_MODULE_PTR     0x48   /* Shadow RAM -> EMP SR settings module */
#define EMP_PHY_CAPS_PTR      0x19   /* EMP + this -> PHY caps, relative     */
#define PHY_MISC0             0x08   /* Misc0 word within a PHY caps struct  */
#define MISC0_MODULE_QUAL     0x0800 /* bit 11: enable module qualification  */

/* A struct must be big enough to contain Misc0 and small enough to be sane.
 * Real images: 0x0b, 0x0c, 0x0d. */
#define PHY_SIZE_MIN          0x09
#define PHY_SIZE_MAX          0x20
#define MAX_PORTS             8

#define NVM_WORDS             0x8000 /* Shadow RAM, in words */
#define CHUNK_WORDS           256    /* per ioctl; AQ buffer is 4KB */

struct layout {
	uint16_t base;    /* word offset of PHY caps struct 0 */
	uint16_t size;    /* payload words, from word 0 of the struct */
	uint16_t stride;  /* size + 1 */
	int count;        /* structs found */
};

static void die(const char *why)
{
	perror(why);
	exit(EXIT_FAILURE);
}

__attribute__((format(printf, 1, 2), noreturn))
static void bail(const char *fmt, ...)
{
	va_list ap;

	fputs("error: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

/* ---- structure recognition: pure, covered by -t ---- */

static uint16_t misc0_word(const struct layout *l, int idx)
{
	return (uint16_t)(l->base + PHY_MISC0 + l->stride * idx);
}

static uint16_t misc0_set_qual(uint16_t misc, int on)
{
	return on ? (uint16_t)(misc | MISC0_MODULE_QUAL)
	          : (uint16_t)(misc & (uint16_t)~MISC0_MODULE_QUAL);
}

/* Words +0x02/+0x03 are the 32-bit phy_type bitmap, low word first. Verified
 * against two independent cards: an X710 decodes to SFI/10GBASE_SR/LR/SFPP_CU/
 * 1000BASE_SX/LX, an XL710 to XLPPI/40GBASE_CR4_CU/CR4/SR4/LR4. */
static uint32_t phy_type_of(const uint16_t *nvm, const struct layout *l, int idx)
{
	uint16_t b = (uint16_t)(l->base + l->stride * idx);
	return (uint32_t)nvm[b + 3] << 16 | nvm[b + 2];
}

static const char *phy_type_name(int bit)
{
	static const char *const n[32] = {
		"SGMII", "1000BASE_KX", "10GBASE_KX4", "10GBASE_KR",
		"40GBASE_KR4", "XAUI", "XFI", "SFI", "XLAUI", "XLPPI",
		"40GBASE_CR4_CU", "10GBASE_CR1_CU", "10GBASE_AOC",
		"40GBASE_AOC", NULL, NULL, NULL, "100BASE_TX", "1000BASE_T",
		"10GBASE_T", "10GBASE_SR", "10GBASE_LR", "10GBASE_SFPP_CU",
		"10GBASE_CR1", "40GBASE_CR4", "40GBASE_SR4", "40GBASE_LR4",
		"1000BASE_SX", "1000BASE_LX", "1000BASE_T_OPTICAL",
		"20GBASE_KR2", NULL,
	};
	return n[bit];
}

/* Low byte of Misc0 is the link_speed bitmap, same encoding as the admin
 * queue's I40E_LINK_SPEED_*_SHIFT values. */
static const char *link_speed_name(int bit)
{
	static const char *const n[8] = {
		NULL, "100M", "1G", "10G", "40G", "20G", "25G", NULL,
	};
	return n[bit];
}

/* Does a PHY capabilities section start at `base`?
 *
 * The size word must repeat at size+1 intervals. That single test rejects the
 * classic false positive: firmware 8.x has the constant 0x000b at struct
 * offset +07, which is exactly the size word of firmware 5.x images, so
 * grepping for 000b lands on base+7. From there the implied stride is 0x0c and
 * the next size word is predicted at a word that holds something else.
 */
static int recognise(const uint16_t *nvm, size_t nwords, uint16_t base,
                     struct layout *out)
{
	uint16_t size, stride;
	int count, i;

	if (base >= nwords)
		return 0;

	size = nvm[base];
	if (size < PHY_SIZE_MIN || size > PHY_SIZE_MAX)
		return 0;
	stride = (uint16_t)(size + 1);

	/* A run of one repeated value satisfies any stride test. Require the
	 * struct to have some internal structure. */
	if (nvm[base + 1] == size)
		return 0;

	for (count = 1; count < MAX_PORTS; count++) {
		size_t at = (size_t)base + (size_t)stride * count;
		if (at + stride > nwords || nvm[at] != size)
			break;
	}
	if (count < 2)
		return 0;

	/* Misc0 must look like flags, not padding, and must agree across
	 * ports - every real dump has all ports identical here. */
	for (i = 0; i < count; i++) {
		uint16_t m = nvm[base + PHY_MISC0 + (size_t)stride * i];
		if (m == 0x0000 || m == 0xffff || m == size)
			return 0;
	}

	out->base = base;
	out->size = size;
	out->stride = stride;
	out->count = count;
	return 1;
}

/* Scan for candidate sections, best first (most ports, then lowest offset).
 * Returns how many were found, which may exceed `max`. */
static int scan(const uint16_t *nvm, size_t nwords, struct layout *out, int max)
{
	size_t w = 0;
	int found = 0;

	while (w < nwords) {
		struct layout cur;
		int i, at;

		if (!recognise(nvm, nwords, (uint16_t)w, &cur)) {
			w++;
			continue;
		}

		/* insertion sort into out[], best first */
		for (at = 0; at < found && at < max; at++)
			if (cur.count > out[at].count)
				break;
		for (i = (found < max ? found : max - 1); i > at; i--)
			out[i] = out[i - 1];
		if (at < max)
			out[at] = cur;

		found++;
		/* Skip past this section so its own structs don't re-match. */
		w += (size_t)cur.stride * cur.count;
	}

	return found;
}

/* ---- selftest, built from the real dumps in the issue tracker ---- */

static void put_struct(uint16_t *nvm, uint16_t at, uint16_t size,
                       const uint16_t *body)
{
	nvm[at] = size;
	memcpy(&nvm[at + 1], body, size * sizeof *body);
}

static void selftest(void)
{
	static uint16_t nvm[NVM_WORDS];
	struct layout l, cand[8];
	int i;

	/* XL710-QDA2 (0x1583), terpstra issue #2: base 0x67fa, size 0x0b,
	 * stride 0x0c, Misc0 0x0b10 (bit 11 set = locked). */
	static const uint16_t qda2[] = { 0x0023, 0x0600, 0x0700, 0x0000,
	                                 0x0000, 0x0e0d, 0x0000, 0x0b10,
	                                 0x0000, 0x0a1e, 0x0002 };
	memset(nvm, 0, sizeof nvm);
	for (i = 0; i < 4; i++)
		put_struct(nvm, (uint16_t)(0x67fa + 0x0c * i), 0x0b, qda2);

	assert(recognise(nvm, NVM_WORDS, 0x67fa, &l));
	assert(l.size == 0x0b && l.stride == 0x0c && l.count == 4);
	assert(misc0_word(&l, 0) == 0x6802);
	assert(nvm[misc0_word(&l, 0)] == 0x0b10);
	assert(nvm[misc0_word(&l, 3)] == 0x0b10);
	assert(scan(nvm, NVM_WORDS, cand, 8) == 1 && cand[0].base == 0x67fa);
	assert(misc0_set_qual(0x0b10, 0) == 0x0310); /* Yaugen-D's value */
	/* 40G-only QSFP+ port: XLPPI, 40GBASE_CR4_CU, CR4, SR4, LR4 */
	assert(phy_type_of(nvm, &l, 0) == 0x07000600);
	assert((nvm[misc0_word(&l, 0)] & 0xff) == 0x10); /* 40G */

	/* X710 firmware 8.13, terpstra issue #9: base 0x6940, size 0x0d,
	 * stride 0x0e, Misc0 0x6b0c. Note 0x000b sitting at +07 - the decoy
	 * that makes people patch base+7. */
	static const uint16_t fw813[] = { 0x0222, 0x0083, 0x1871, 0x0000,
	                                  0x0000, 0x3303, 0x000b, 0x6b0c,
	                                  0x0a00, 0x0a1e, 0x0003, 0x0000,
	                                  0x0064 };
	memset(nvm, 0, sizeof nvm);
	for (i = 0; i < 4; i++)
		put_struct(nvm, (uint16_t)(0x6940 + 0x0e * i), 0x0d, fw813);

	assert(recognise(nvm, NVM_WORDS, 0x6940, &l));
	assert(l.size == 0x0d && l.stride == 0x0e && l.count == 4);
	assert(nvm[misc0_word(&l, 0)] == 0x6b0c);
	assert(misc0_set_qual(0x6b0c, 0) == 0x630c);
	/* SFP+ port: SGMII, 1000BASE_KX, SFI, 10GBASE_SR/LR/SFPP_CU, 1000BASE_SX/LX */
	assert(phy_type_of(nvm, &l, 0) == 0x18710083);
	assert((nvm[misc0_word(&l, 0)] & 0xff) == 0x0c); /* 1G + 10G */
	/* the decoy must not validate */
	assert(!recognise(nvm, NVM_WORDS, 0x6947, &l));
	assert(scan(nvm, NVM_WORDS, cand, 8) == 1 && cand[0].base == 0x6940);

	/* X710 firmware 6.x, gelezayka: base 0x68f0, size 0x0c, stride 0x0d,
	 * Misc0 0x630c - already clear, must report unlocked not locked. */
	static const uint16_t fw6[] = { 0x0022, 0x0083, 0x1871, 0x0000,
	                                0x0000, 0x3303, 0x000b, 0x630c,
	                                0x0a00, 0x0a1e, 0x0003, 0x0000 };
	memset(nvm, 0, sizeof nvm);
	for (i = 0; i < 4; i++)
		put_struct(nvm, (uint16_t)(0x68f0 + 0x0d * i), 0x0c, fw6);

	assert(recognise(nvm, NVM_WORDS, 0x68f0, &l));
	assert(l.size == 0x0c && l.stride == 0x0d && l.count == 4);
	assert(!(nvm[misc0_word(&l, 0)] & MISC0_MODULE_QUAL));

	/* Padding and constant runs must never validate. */
	memset(nvm, 0, sizeof nvm);
	assert(scan(nvm, NVM_WORDS, cand, 8) == 0);
	for (i = 0; i < 0x400; i++)
		nvm[0x5000 + i] = 0x000c;
	assert(!recognise(nvm, NVM_WORDS, 0x5000, &l));
	memset(&nvm[0x5000], 0xff, 0x400 * sizeof *nvm);
	assert(!recognise(nvm, NVM_WORDS, 0x5000, &l));

	/* Bit twiddling is idempotent, unlike the XOR it replaces. */
	assert(misc0_set_qual(0x630c, 0) == 0x630c);
	assert(misc0_set_qual(0x630c, 1) == 0x6b0c);
	assert(misc0_set_qual(0x6b0c, 1) == 0x6b0c);

	printf("selftest: ok\n");
}

/* ---- NVM access ---- */

struct nvm {
	int fd;
	const char *ifname;
	uint32_t devid;
};

static int nvm_op(struct nvm *n, uint32_t op, uint32_t trans,
                  uint32_t byte_off, uint32_t len, void *data)
{
	struct {
		uint32_t cmd, magic, offset, len;
		uint8_t data[CHUNK_WORDS * 2];
	} req = {
		.cmd    = op,
		.magic  = (n->devid << 16) | (trans << I40E_NVM_TRANS_SHIFT),
		.offset = byte_off,
		.len    = len,
	};
	struct ifreq ifr;

	assert(len >= 1 && len <= sizeof req.data);
	if (op == I40E_NVM_WRITE && data)
		memcpy(req.data, data, len);

	memset(&ifr, 0, sizeof ifr);
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", n->ifname);
	ifr.ifr_data = (void *)&req;

	if (ioctl(n->fd, SIOCETHTOOL, &ifr) == -1)
		return -1;

	if (op == I40E_NVM_READ && data)
		memcpy(data, req.data, len);
	return 0;
}

static void nvm_slurp(struct nvm *n, uint16_t *buf, size_t nwords)
{
	size_t w;

	for (w = 0; w < nwords; w += CHUNK_WORDS) {
		size_t chunk = nwords - w < CHUNK_WORDS ? nwords - w : CHUNK_WORDS;
		if (nvm_op(n, I40E_NVM_READ, I40E_NVM_SA,
		           (uint32_t)(w << 1), (uint32_t)(chunk << 1), &buf[w]))
			die("NVM read");
	}
}

static uint16_t rd_word(struct nvm *n, uint16_t word)
{
	uint16_t v;
	if (nvm_op(n, I40E_NVM_READ, I40E_NVM_SA, (uint32_t)word << 1, 2, &v))
		die("NVM read");
	return v;
}

static void wr_word(struct nvm *n, uint16_t word, uint16_t v)
{
	if (nvm_op(n, I40E_NVM_WRITE, I40E_NVM_SA, (uint32_t)word << 1, 2, &v))
		die("NVM write");
}

static void update_checksum(struct nvm *n)
{
	uint16_t zero = 0;
	if (nvm_op(n, I40E_NVM_WRITE, I40E_NVM_CSUM | I40E_NVM_SA, 0, 2, &zero))
		die("NVM checksum update");
}

/* ---- device identification ---- */

/* Read the first NVM_WORDS words of an NVM image. Intel's update .bin files
 * are the whole 4MB flash; only the leading shadow RAM concerns us. */
static size_t load_image(const char *path, uint16_t *buf)
{
	FILE *f = fopen(path, "rb");
	size_t got;

	if (!f)
		die(path);
	got = fread(buf, sizeof *buf, NVM_WORDS, f);
	fclose(f);
	if (got < 0x100)
		bail("%s: only %zu words, too short to be an NVM image",
		     path, got);
	return got;
}

static unsigned long sysfs_hex(const char *ifname, const char *attr)
{
	char path[256], buf[64];
	FILE *f;

	snprintf(path, sizeof path, "/sys/class/net/%s/device/%s", ifname, attr);
	f = fopen(path, "r");
	if (!f)
		die(path);
	if (!fgets(buf, sizeof buf, f))
		die(path);
	fclose(f);
	return strtoul(buf, NULL, 16);
}

static void check_driver(const char *ifname)
{
	char path[256], link[256];
	ssize_t len;
	const char *drv;

	snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", ifname);
	len = readlink(path, link, sizeof link - 1);
	if (len < 0)
		die(path);
	link[len] = '\0';

	drv = strrchr(link, '/');
	drv = drv ? drv + 1 : link;
	if (strcmp(drv, "i40e") != 0)
		bail("%s is driven by '%s', not i40e - refusing to write its NVM",
		     ifname, drv);
}

static int known_devid(uint32_t id)
{
	static const uint32_t ids[] = {
		0x1572, 0x1580, 0x1581, 0x1583, 0x1584, 0x1585, 0x1586,
		0x1587, 0x1588, 0x1589, 0x158a, 0x158b, 0x15ff,
	};
	for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
		if (ids[i] == id)
			return 1;
	return 0;
}

/* ---- reporting ---- */

static void dump(const uint16_t *nvm, uint16_t at, int count)
{
	for (int i = 0; i < count; i++)
		printf("  %04x + %02x => %04x\n", at, i, nvm[at + i]);
}

static uint32_t eetrack_of(const uint16_t *nvm)
{
	return (uint32_t)nvm[SR_NVM_EETRACK_HI] << 16 | nvm[SR_NVM_EETRACK_LO];
}

/* Diff this card's PHY capabilities against a reference NVM image, e.g. one of
 * the .bin files in Intel's NVM update package. A card whose struct does not
 * match any shipped image is in a state no firmware expects, which is a more
 * likely explanation for odd behaviour than a wrong offset. */
static void compare(const uint16_t *card, const struct layout *cl,
                    const uint16_t *ref, size_t rn, const char *name)
{
	struct layout rl;
	int diffs = 0;

	printf("\nvs %s:\n", name);
	printf("  EETRACK   card 0x%08x   image 0x%08x%s\n",
	       eetrack_of(card), eetrack_of(ref),
	       eetrack_of(card) == eetrack_of(ref) ? "   (same build)" : "");

	if (!recognise(ref, rn, cl->base, &rl)) {
		printf("  no PHY capabilities at 0x%04x in the image; "
		       "layouts differ, not comparing structs\n", cl->base);
		return;
	}
	if (rl.size != cl->size || rl.stride != cl->stride) {
		printf("  layout differs (image size 0x%02x stride 0x%02x); "
		       "not comparing structs\n", rl.size, rl.stride);
		return;
	}

	for (int p = 0; p < cl->count && p < rl.count; p++) {
		uint16_t cb = (uint16_t)(cl->base + cl->stride * p);
		uint16_t rb = (uint16_t)(rl.base + rl.stride * p);

		for (int i = 0; i < cl->stride; i++)
			if (card[cb + i] != ref[rb + i]) {
				printf("  port %d +%02x: card %04x  image %04x%s\n",
				       p, i, card[cb + i], ref[rb + i],
				       i == PHY_MISC0 ? "   <- Misc0" : "");
				diffs++;
			}
	}
	if (!diffs)
		printf("  PHY capabilities structs are identical\n");
}

static void show_layout(const uint16_t *nvm, const struct layout *l)
{
	printf("EETRACK 0x%08x\n", eetrack_of(nvm));
	printf("PHY capabilities at 0x%04x: %d struct(s), "
	       "size 0x%02x, stride 0x%02x\n",
	       l->base, l->count, l->size, l->stride);

	for (int i = 0; i < l->count; i++) {
		uint16_t w = misc0_word(l, i);
		printf("  port %d  struct 0x%04x  Misc0 @ 0x%04x = 0x%04x  %s\n",
		       i, (uint16_t)(l->base + l->stride * i), w, nvm[w],
		       (nvm[w] & MISC0_MODULE_QUAL) ? "locked" : "unlocked");
	}

	/* What the card will actually accept, independent of qualification.
	 * A module outside these lists is refused no matter what bit 11 says. */
	uint32_t pt = phy_type_of(nvm, l, 0);
	uint16_t speeds = nvm[misc0_word(l, 0)] & 0xff;

	printf("  phy_type 0x%08x:", pt);
	for (int b = 0; b < 32; b++)
		if ((pt >> b & 1) && phy_type_name(b))
			printf(" %s", phy_type_name(b));
	printf("\n  link_speed 0x%02x:", speeds);
	for (int b = 0; b < 8; b++)
		if ((speeds >> b & 1) && link_speed_name(b))
			printf(" %s", link_speed_name(b));
	printf("\n");
}

static void usage(void)
{
	fprintf(stderr,
	    "xl710_unlock - allow unsupported SFP/QSFP modules on Intel X710/XL710\n"
	    "\n"
	    "  xl710_unlock -n <iface> [-u | -l] [-y] [-b <base>] [-i <devid>]\n"
	    "  xl710_unlock -n <iface> -d <word> [-N <count>]\n"
	    "  xl710_unlock -f <dump> [-d <word>] [-N <count>]\n"
	    "  xl710_unlock -t\n"
	    "\n"
	    "  -n <iface>   interface to operate on (required unless -f)\n"
	    "  -f <dump>    analyse a saved image instead of a card, read-only.\n"
	    "               Make one with: ethtool -e <iface> raw on > nvm.bin\n"
	    "  -c <image>   also diff against a reference NVM image, e.g. a .bin\n"
	    "               from Intel's NVM update package\n"
	    "  -u           unlock: accept unsupported modules\n"
	    "  -l           lock: restore Intel's qualified-module check\n"
	    "  -y           don't ask for confirmation\n"
	    "  -b <base>    force the PHY capabilities base word offset\n"
	    "  -d <word>    dump NVM words starting here, then exit\n"
	    "  -N <count>   words to dump (default 32)\n"
	    "  -i <devid>   override the PCI device ID from sysfs\n"
	    "  -t           run the selftest against known card dumps, then exit\n"
	    "\n"
	    "With neither -u nor -l it only reports the current state.\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *const *argv)
{
	const char *ifname = NULL, *fromfile = NULL, *reffile = NULL;
	uint32_t devid = 0;
	long force_base = -1, dump_at = -1, dump_n = 32;
	int want_lock = -1, assume_yes = 0, c;
	size_t nwords = NVM_WORDS;

	while ((c = getopt(argc, argv, "n:i:b:d:N:f:c:ulyth?")) != -1) {
		switch (c) {
		case 'n': ifname = optarg; break;
		case 'f': fromfile = optarg; break;
		case 'c': reffile = optarg; break;
		case 'i': devid = strtoul(optarg, NULL, 0); break;
		case 'b': force_base = strtol(optarg, NULL, 0); break;
		case 'd': dump_at = strtol(optarg, NULL, 0); break;
		case 'N': dump_n = strtol(optarg, NULL, 0); break;
		case 'u': want_lock = 0; break;
		case 'l': want_lock = 1; break;
		case 'y': assume_yes = 1; break;
		case 't': selftest(); return 0;
		default:  usage();
		}
	}

	if (!ifname && !fromfile)
		usage();

	static uint16_t sr[NVM_WORDS];
	struct nvm nvm = { 0 };

	if (fromfile) {
		if (want_lock >= 0)
			bail("-f analyses a saved image; it cannot write. "
			     "Drop -u/-l and run against the card.");
		nwords = load_image(fromfile, sr);
		printf("%s: %zu words (0x%zx)\n", fromfile, nwords, nwords);
		goto analyse;
	}

	if (geteuid() != 0)
		bail("must run as root to access the NVM");

	check_driver(ifname);

	if (!devid) {
		unsigned long vendor = sysfs_hex(ifname, "vendor");
		if (vendor != 0x8086)
			bail("%s has PCI vendor 0x%04lx, expected Intel (0x8086)",
			     ifname, vendor);
		devid = (uint32_t)sysfs_hex(ifname, "device");
		if (!known_devid(devid))
			bail("unrecognised device ID 0x%04x; pass -i 0x%04x to "
			     "proceed anyway", devid, devid);
	}

	nvm.ifname = ifname;
	nvm.devid = devid;
	nvm.fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (nvm.fd == -1)
		die("socket");

	printf("%s: device 0x%04x\n", ifname, devid);
	nvm_slurp(&nvm, sr, NVM_WORDS);

analyse:
	if (dump_at >= 0) {
		if ((size_t)dump_at >= nwords || dump_n < 1 ||
		    (size_t)(dump_at + dump_n) > nwords)
			bail("dump range outside the image (0..0x%zx)",
			     nwords - 1);
		dump(sr, (uint16_t)dump_at, (int)dump_n);
		return 0;
	}

	/* The pointer chain is a hint. Report what it claims, then insist the
	 * signature agrees before trusting it. */
	struct layout l;
	uint16_t emp = sr[SR_EMP_MODULE_PTR];
	long hinted = -1;

	if (emp && emp != 0xffff && (size_t)emp + EMP_PHY_CAPS_PTR < nwords) {
		hinted = sr[emp + EMP_PHY_CAPS_PTR] + emp + EMP_PHY_CAPS_PTR;
		printf("EMP SR at 0x%04x, pointer chain suggests 0x%04lx\n",
		       emp, hinted);
	} else {
		printf("EMP SR pointer unusable (0x%04x)\n", emp);
	}

	if (force_base >= 0) {
		if (!recognise(sr, nwords, (uint16_t)force_base, &l))
			bail("no PHY capabilities structure at 0x%04lx "
			     "(use -d 0x%04lx to look at it)",
			     force_base, force_base);
		printf("using forced base 0x%04lx\n", force_base);
	} else if (hinted >= 0 && (size_t)hinted < nwords &&
	           recognise(sr, nwords, (uint16_t)hinted, &l)) {
		printf("pointer chain validated\n");
	} else {
		struct layout cand[8];
		int n = scan(sr, nwords, cand, 8);

		if (hinted >= 0)
			printf("pointer chain does NOT point at a valid "
			       "structure - scanning instead\n");
		if (n == 0)
			bail("no PHY capabilities structure found; "
			     "dump the NVM with -d and inspect it by hand");

		l = cand[0];
		if (n > 1) {
			printf("%d candidate sections:\n", n);
			for (int i = 0; i < n && i < 8; i++)
				printf("    0x%04x  %d struct(s)  size 0x%02x  "
				       "Misc0 0x%04x%s\n",
				       cand[i].base, cand[i].count, cand[i].size,
				       sr[misc0_word(&cand[i], 0)],
				       i == 0 ? "  <- using this, override with -b"
				              : "");
		} else {
			printf("found by scan at 0x%04x\n", l.base);
		}
	}

	show_layout(sr, &l);

	if (reffile) {
		static uint16_t ref[NVM_WORDS];
		size_t rn = load_image(reffile, ref);
		compare(sr, &l, ref, rn, reffile);
	}

	int locked = 0;
	for (int i = 0; i < l.count; i++)
		if (sr[misc0_word(&l, i)] & MISC0_MODULE_QUAL)
			locked++;

	if (want_lock < 0) {
		printf("\n%d of %d struct(s) locked. Pass -u to unlock.\n",
		       locked, l.count);
		if (!locked)
			printf("Bit 11 is already clear in the NVM. If the card "
			       "still rejects modules, the NVM is not the\n"
			       "reason: power-cycle at the wall first (the EMP "
			       "only re-reads this section at power-on),\n"
			       "then check `dmesg | grep i40e` for the actual "
			       "message.\n");
		return 0;
	}

	int todo = want_lock ? l.count - locked : locked;
	if (!todo) {
		printf("\nAlready %s, nothing to do.\n",
		       want_lock ? "locked" : "unlocked");
		return 0;
	}

	printf("\nAbout to %s %d struct(s) in the NVM of %s.\n",
	       want_lock ? "lock" : "unlock", todo, ifname);
	printf("To undo, run: %s -n %s %s\n", argv[0], ifname,
	       want_lock ? "-u" : "-l");

	if (!assume_yes) {
		char line[16];
		printf("Continue? [y/N]: ");
		fflush(stdout);
		if (!fgets(line, sizeof line, stdin) ||
		    (line[0] != 'y' && line[0] != 'Y')) {
			printf("aborted\n");
			return 1;
		}
	}

	int written = 0;
	for (int i = 0; i < l.count; i++) {
		uint16_t w = misc0_word(&l, i);
		uint16_t old = sr[w];
		uint16_t want = misc0_set_qual(old, want_lock);

		if (want == old)
			continue;

		wr_word(&nvm, w, want);
		sleep(1); /* the admin queue needs a moment between writes */

		uint16_t back = rd_word(&nvm, w);
		if (back != want)
			bail("port %d @ 0x%04x: wrote 0x%04x, read back 0x%04x",
			     i, w, want, back);

		printf("  port %d @ 0x%04x: 0x%04x -> 0x%04x\n", i, w, old, back);
		written++;
	}

	if (written) {
		update_checksum(&nvm);
		printf("\n%d struct(s) patched, NVM checksum updated.\n", written);
		printf("Power-cycle the machine (a warm reboot is not enough) "
		       "for the change to take effect.\n");
	}

	return 0;
}
