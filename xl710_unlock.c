/* xl710_unlock - clear the "qualified module only" bit in Intel X710/XL710 NVM.
 *
 * Based on bibigon812/xl710-unlocker, corrected for XL710-QDA2 (0x1583):
 *
 *   - device ID comes from sysfs instead of a hardcoded 0x1572. i40e's read
 *     path ignores the device ID in the magic but the write path rejects a
 *     mismatch with -EINVAL, so the original reports fine then fails to write.
 *   - each PHY struct is read-modify-written from its own value, not from
 *     whatever the last read left in misc0.
 *   - the bit is cleared/set explicitly instead of XORed (XOR re-locks a
 *     struct that was already unlocked).
 *   - every write is read back and verified; empty/garbage structs are skipped
 *     instead of written to.
 *   - refuses to touch anything that is not an i40e device.
 */

#include <assert.h>
#include <errno.h>
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

/* Shadow RAM word offsets */
#define SR_EMP_MODULE_PTR     0x48   /* -> EMP SR settings module          */
#define EMP_PHY_CAPS_PTR      0x19   /* -> PHY capabilities, EMP-relative  */
#define PHY_MISC_WORD         0x08   /* MISC word inside a PHY caps struct */
#define MISC_MODULE_QUAL      0x0800 /* 1 = accept qualified modules only  */

/* Sanity bounds: NVM images are a few hundred KB, structs are ~12 words. */
#define PHY_SIZE_MIN          0x04
#define PHY_SIZE_MAX          0x40

struct nvm {
	int fd;
	const char *ifname;
	uint32_t devid;
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

/* ---- pure helpers, covered by -t ---- */

static uint16_t phy_misc_word(uint16_t phy_off, uint16_t size, int idx)
{
	/* Each struct is preceded by its own size word, hence the stride +1. */
	return (uint16_t)(phy_off + PHY_MISC_WORD + (size + 1) * idx);
}

static uint16_t misc_set_qual(uint16_t misc, int on)
{
	return on ? (uint16_t)(misc | MISC_MODULE_QUAL)
	          : (uint16_t)(misc & (uint16_t)~MISC_MODULE_QUAL);
}

static int misc_plausible(uint16_t misc)
{
	return misc != 0x0000 && misc != 0xffff;
}

static void selftest(void)
{
	/* Offsets from the upstream README's X710-DA2 dump. */
	assert(phy_misc_word(0x68f6, 0x000c, 0) == 0x68fe);
	assert(phy_misc_word(0x68f6, 0x000c, 1) == 0x690b);
	assert(phy_misc_word(0x68f6, 0x000c, 3) == 0x6925);

	assert(misc_set_qual(0x6b0c, 0) == 0x630c);   /* locked   -> unlocked */
	assert(misc_set_qual(0x630c, 0) == 0x630c);   /* idempotent, unlike ^ */
	assert(misc_set_qual(0x630c, 1) == 0x6b0c);   /* unlocked -> locked   */
	assert(misc_set_qual(0x6b0c, 1) == 0x6b0c);

	assert(!misc_plausible(0x0000) && !misc_plausible(0xffff));
	assert(misc_plausible(0x6b0c));

	printf("selftest: ok\n");
}

/* ---- NVM access ---- */

static int nvm_op(struct nvm *n, uint32_t op, uint32_t trans,
                  uint32_t byte_off, uint32_t len, void *data)
{
	/* ponytail: 8-byte payload, every op here is a single word. */
	struct {
		uint32_t cmd, magic, offset, len;
		uint8_t data[8];
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

/* i40e device IDs that carry a PHY capabilities section. */
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

static void usage(void)
{
	fprintf(stderr,
	    "xl710_unlock - allow unsupported SFP/QSFP modules on Intel X710/XL710\n"
	    "\n"
	    "  xl710_unlock -n <iface> [-u | -l] [-y] [-i <devid>] [-c <structs>]\n"
	    "\n"
	    "  -n <iface>    interface to operate on (required)\n"
	    "  -u            unlock: accept unsupported modules\n"
	    "  -l            lock: restore Intel's qualified-module check\n"
	    "  -y            don't ask for confirmation\n"
	    "  -i <devid>    override the PCI device ID from sysfs\n"
	    "  -c <structs>  PHY capability structs to patch (default 4)\n"
	    "  -t            run the offset/bit selftest and exit\n"
	    "\n"
	    "With neither -u nor -l it only reports the current state.\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *const *argv)
{
	const char *ifname = NULL;
	uint32_t devid = 0;
	int want_lock = -1, assume_yes = 0, nstructs = 4, c;

	while ((c = getopt(argc, argv, "n:i:c:ulyth?")) != -1) {
		switch (c) {
		case 'n': ifname = optarg; break;
		case 'i': devid = strtoul(optarg, NULL, 0); break;
		case 'c': nstructs = atoi(optarg); break;
		case 'u': want_lock = 0; break;
		case 'l': want_lock = 1; break;
		case 'y': assume_yes = 1; break;
		case 't': selftest(); return 0;
		default:  usage();
		}
	}

	if (!ifname || nstructs < 1 || nstructs > 8)
		usage();
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

	struct nvm nvm = { .ifname = ifname, .devid = devid };
	nvm.fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (nvm.fd == -1)
		die("socket");

	printf("%s: device 0x%04x\n", ifname, devid);

	/* Shadow RAM -> EMP SR module -> PHY capabilities section. */
	uint16_t emp = rd_word(&nvm, SR_EMP_MODULE_PTR);
	if (!emp || emp == 0xffff)
		bail("bogus EMP SR pointer 0x%04x", emp);
	printf("EMP SR offset:        0x%04x\n", emp);

	uint16_t phy = (uint16_t)(rd_word(&nvm, emp + EMP_PHY_CAPS_PTR)
	                          + emp + EMP_PHY_CAPS_PTR);
	if (phy <= emp)
		bail("bogus PHY capabilities pointer 0x%04x", phy);
	printf("PHY caps offset:      0x%04x\n", phy);

	uint16_t size = rd_word(&nvm, phy);
	if (size < PHY_SIZE_MIN || size > PHY_SIZE_MAX)
		bail("implausible PHY struct size 0x%04x - wrong NVM layout, "
		     "not writing anything", size);
	printf("PHY struct size:      0x%04x words\n", size);

	/* Read every struct on its own before deciding anything. */
	uint16_t misc[8], word[8];
	int locked = 0, differ = 0, usable = 0;

	for (int i = 0; i < nstructs; i++) {
		word[i] = phy_misc_word(phy, size, i);
		misc[i] = rd_word(&nvm, word[i]);

		printf("  struct %d @ 0x%04x  MISC 0x%04x  %s\n", i, word[i],
		       misc[i],
		       !misc_plausible(misc[i]) ? "empty, skipping" :
		       (misc[i] & MISC_MODULE_QUAL) ? "locked" : "unlocked");

		if (!misc_plausible(misc[i]))
			continue;
		usable++;
		if (misc[i] & MISC_MODULE_QUAL)
			locked++;
		if (misc[i] != misc[0])
			differ = 1;
	}

	if (!usable)
		bail("no usable PHY capability struct found");
	if (differ)
		printf("note: MISC values differ between structs; "
		       "each is patched from its own value\n");

	if (want_lock < 0) {
		printf("\n%d of %d struct(s) locked. Pass -u to unlock.\n",
		       locked, usable);
		return 0;
	}

	int todo = want_lock ? usable - locked : locked;
	if (!todo) {
		printf("\nAlready %s, nothing to do.\n",
		       want_lock ? "locked" : "unlocked");
		return 0;
	}

	printf("\nAbout to %s %d struct(s) in the NVM of %s.\n",
	       want_lock ? "lock" : "unlock", todo, ifname);
	printf("To undo, run: xl710_unlock -n %s %s\n", ifname,
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
	for (int i = 0; i < nstructs; i++) {
		if (!misc_plausible(misc[i]))
			continue;

		uint16_t want = misc_set_qual(misc[i], want_lock);
		if (want == misc[i])
			continue;

		wr_word(&nvm, word[i], want);
		sleep(1); /* the admin queue needs a moment between writes */

		uint16_t back = rd_word(&nvm, word[i]);
		if (back != want)
			bail("struct %d @ 0x%04x: wrote 0x%04x, read back 0x%04x",
			     i, word[i], want, back);

		printf("  struct %d @ 0x%04x: 0x%04x -> 0x%04x\n",
		       i, word[i], misc[i], back);
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
