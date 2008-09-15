/* Copyright (c) 2008 The Board of Trustees of The Leland Stanford
 * Junior University
 * 
 * We are making the OpenFlow specification and associated documentation
 * (Software) available for public use and benefit with the expectation
 * that others will use, modify and enhance the Software and contribute
 * those enhancements back to the community. However, since we would
 * like to make the Software available for broadest use, with as few
 * restrictions as possible permission is hereby granted, free of
 * charge, to any person obtaining a copy of this Software to deal in
 * the Software under the copyrights without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * The name and trademarks of copyright holder(s) may NOT be used in
 * advertising or publicity pertaining to the Software or any
 * derivatives without specific, written prior permission.
 */

#include <config.h>
#include "ofp-print.h"
#include "xtoxll.h"

#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>

#include "compiler.h"
#include "dynamic-string.h"
#include "flow.h"
#include "ofpbuf.h"
#include "openflow.h"
#include "packets.h"
#include "util.h"

static void ofp_print_port_name(struct ds *string, uint16_t port);
static void ofp_print_match(struct ds *, const struct ofp_match *,
                            int verbosity);

/* Returns a string that represents the contents of the Ethernet frame in the
 * 'len' bytes starting at 'data' to 'stream' as output by tcpdump.
 * 'total_len' specifies the full length of the Ethernet frame (of which 'len'
 * bytes were captured).
 *
 * The caller must free the returned string.
 *
 * This starts and kills a tcpdump subprocess so it's quite expensive. */
char *
ofp_packet_to_string(const void *data, size_t len, size_t total_len)
{
    struct pcap_hdr {
        uint32_t magic_number;   /* magic number */
        uint16_t version_major;  /* major version number */
        uint16_t version_minor;  /* minor version number */
        int32_t thiszone;        /* GMT to local correction */
        uint32_t sigfigs;        /* accuracy of timestamps */
        uint32_t snaplen;        /* max length of captured packets */
        uint32_t network;        /* data link type */
    } PACKED;

    struct pcaprec_hdr {
        uint32_t ts_sec;         /* timestamp seconds */
        uint32_t ts_usec;        /* timestamp microseconds */
        uint32_t incl_len;       /* number of octets of packet saved in file */
        uint32_t orig_len;       /* actual length of packet */
    } PACKED;

    struct pcap_hdr ph;
    struct pcaprec_hdr prh;

    struct ds ds = DS_EMPTY_INITIALIZER;

    char command[128];
    FILE *pcap;
    FILE *tcpdump;
    int status;
    int c;

    pcap = tmpfile();
    if (!pcap) {
        ofp_error(errno, "tmpfile");
        return xstrdup("<error>");
    }

    /* The pcap reader is responsible for figuring out endianness based on the
     * magic number, so the lack of htonX calls here is intentional. */
    ph.magic_number = 0xa1b2c3d4;
    ph.version_major = 2;
    ph.version_minor = 4;
    ph.thiszone = 0;
    ph.sigfigs = 0;
    ph.snaplen = 1518;
    ph.network = 1;             /* Ethernet */

    prh.ts_sec = 0;
    prh.ts_usec = 0;
    prh.incl_len = len;
    prh.orig_len = total_len;

    fwrite(&ph, 1, sizeof ph, pcap);
    fwrite(&prh, 1, sizeof prh, pcap);
    fwrite(data, 1, len, pcap);

    fflush(pcap);
    if (ferror(pcap)) {
        ofp_error(errno, "error writing temporary file");
    }
    rewind(pcap);

    snprintf(command, sizeof command, "tcpdump -n -r /dev/fd/%d 2>/dev/null",
             fileno(pcap));
    tcpdump = popen(command, "r");
    fclose(pcap);
    if (!tcpdump) {
        ofp_error(errno, "exec(\"%s\")", command);
        return xstrdup("<error>");
    }

    while ((c = getc(tcpdump)) != EOF) {
        ds_put_char(&ds, c);
    }

    status = pclose(tcpdump);
    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status))
            ofp_error(0, "tcpdump exited with status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        ofp_error(0, "tcpdump exited with signal %d", WTERMSIG(status)); 
    }
    return ds_cstr(&ds);
}

/* Pretty-print the OFPT_PACKET_IN packet of 'len' bytes at 'oh' to 'stream'
 * at the given 'verbosity' level. */
static void
ofp_packet_in(struct ds *string, const void *oh, size_t len, int verbosity)
{
    const struct ofp_packet_in *op = oh;
    size_t data_len;

    ds_put_format(string, " total_len=%"PRIu16" in_port=",
                  ntohs(op->total_len));
    ofp_print_port_name(string, ntohs(op->in_port));

    if (op->reason == OFPR_ACTION)
        ds_put_cstr(string, " (via action)");
    else if (op->reason != OFPR_NO_MATCH)
        ds_put_format(string, " (***reason %"PRIu8"***)", op->reason);

    data_len = len - offsetof(struct ofp_packet_in, data);
    ds_put_format(string, " data_len=%zu", data_len);
    if (htonl(op->buffer_id) == UINT32_MAX) {
        ds_put_format(string, " (unbuffered)");
        if (ntohs(op->total_len) != data_len)
            ds_put_format(string, " (***total_len != data_len***)");
    } else {
        ds_put_format(string, " buffer=0x%08"PRIx32, ntohl(op->buffer_id));
        if (ntohs(op->total_len) < data_len)
            ds_put_format(string, " (***total_len < data_len***)");
    }
    ds_put_char(string, '\n');

    if (verbosity > 0) {
        struct flow flow;
        struct ofpbuf packet;
        struct ofp_match match;
        packet.data = (void *) op->data;
        packet.size = data_len;
        flow_extract(&packet, ntohs(op->in_port), &flow);
        match.wildcards = 0;
        match.in_port = flow.in_port;
        memcpy(match.dl_src, flow.dl_src, ETH_ADDR_LEN);
        memcpy(match.dl_dst, flow.dl_dst, ETH_ADDR_LEN);
        match.dl_vlan = flow.dl_vlan;
        match.dl_type = flow.dl_type;
        match.nw_proto = flow.nw_proto;
        match.pad = 0;
        match.nw_src = flow.nw_src;
        match.nw_dst = flow.nw_dst;
        match.tp_src = flow.tp_src;
        match.tp_dst = flow.tp_dst;
        ofp_print_match(string, &match, verbosity);
        ds_put_char(string, '\n');
    }
    if (verbosity > 1) {
        char *packet = ofp_packet_to_string(op->data, data_len,
                                            ntohs(op->total_len)); 
        ds_put_cstr(string, packet);
        free(packet);
    }
}

static void ofp_print_port_name(struct ds *string, uint16_t port) 
{
    const char *name;
    switch (port) {
    case OFPP_IN_PORT:
        name = "IN_PORT";
        break;
    case OFPP_TABLE:
        name = "TABLE";
        break;
    case OFPP_NORMAL:
        name = "NORMAL";
        break;
    case OFPP_FLOOD:
        name = "FLOOD";
        break;
    case OFPP_ALL:
        name = "ALL";
        break;
    case OFPP_CONTROLLER:
        name = "CONTROLLER";
        break;
    case OFPP_LOCAL:
        name = "LOCAL";
        break;
    case OFPP_NONE:
        name = "NONE";
        break;
    default:
        ds_put_format(string, "%"PRIu16, port);
        return;
    }
    ds_put_cstr(string, name);
}

static void
ofp_print_action(struct ds *string, const struct ofp_action *a) 
{
    switch (ntohs(a->type)) {
    case OFPAT_OUTPUT:
        {
            uint16_t port = ntohs(a->arg.output.port); 
            if (port < OFPP_MAX) {
                ds_put_format(string, "output:%"PRIu16, port);
            } else {
                ofp_print_port_name(string, port);
                if (port == OFPP_CONTROLLER) {
                    if (a->arg.output.max_len) {
                        ds_put_format(string, ":%"PRIu16, 
                                ntohs(a->arg.output.max_len));
                    } else {
                        ds_put_cstr(string, ":all");
                    }
                }
            }
        }
        break;

    case OFPAT_SET_DL_VLAN:
        ds_put_cstr(string, "mod_vlan:");
        if (ntohs(a->arg.vlan_id) == OFP_VLAN_NONE) {
            ds_put_cstr(string, "strip");
        } else {
            ds_put_format(string, "%"PRIu16, ntohs(a->arg.vlan_id));
        }
        break;

    case OFPAT_SET_DL_SRC:
        ds_put_format(string, "mod_dl_src:"ETH_ADDR_FMT, 
                ETH_ADDR_ARGS(a->arg.dl_addr));
        break;

    case OFPAT_SET_DL_DST:
        ds_put_format(string, "mod_dl_dst:"ETH_ADDR_FMT, 
                ETH_ADDR_ARGS(a->arg.dl_addr));
        break;

    case OFPAT_SET_NW_SRC:
        ds_put_format(string, "mod_nw_src:"IP_FMT, IP_ARGS(&a->arg.nw_addr));
        break;

    case OFPAT_SET_NW_DST:
        ds_put_format(string, "mod_nw_dst:"IP_FMT, IP_ARGS(&a->arg.nw_addr));
        break;

    case OFPAT_SET_TP_SRC:
        ds_put_format(string, "mod_tp_src:%d", ntohs(a->arg.tp));
        break;

    case OFPAT_SET_TP_DST:
        ds_put_format(string, "mod_tp_dst:%d", ntohs(a->arg.tp));
        break;

    default:
        ds_put_format(string, "(decoder %"PRIu16" not implemented)", 
                ntohs(a->type));
        break;
    }
}

static void ofp_print_actions(struct ds *string,
                              const struct ofp_action actions[],
                              size_t n_bytes) 
{
    size_t i;
    int n_actions = n_bytes / sizeof *actions;

    ds_put_format(string, "action%s=", n_actions == 1 ? "" : "s");
    for (i = 0; i < n_actions; i++) {
        if (i) {
            ds_put_cstr(string, ",");
        }
        ofp_print_action(string, &actions[i]);
    }
    if (n_bytes % sizeof *actions) {
        if (i) {
            ds_put_cstr(string, ",");
        }
        ds_put_cstr(string, ", ***trailing garbage***");
    }
}

/* Pretty-print the OFPT_PACKET_OUT packet of 'len' bytes at 'oh' to 'string'
 * at the given 'verbosity' level. */
static void ofp_packet_out(struct ds *string, const void *oh, size_t len,
                           int verbosity) 
{
    const struct ofp_packet_out *opo = oh;
    int n_actions = ntohs(opo->n_actions);
    int act_len = n_actions * sizeof opo->actions[0];

    ds_put_cstr(string, " in_port=");
    ofp_print_port_name(string, ntohs(opo->in_port));

    ds_put_format(string, " n_actions=%d ", n_actions);
    if (act_len > (ntohs(opo->header.length) - sizeof *opo)) {
        ds_put_format(string, "***packet too short for number of actions***\n");
        return;
    }
    ofp_print_actions(string, opo->actions, act_len);

    if (ntohl(opo->buffer_id) == UINT32_MAX) {
        int data_len = len - sizeof *opo - act_len;
        ds_put_format(string, " data_len=%d", data_len);
        if (verbosity > 0 && len > sizeof *opo) {
            char *packet = ofp_packet_to_string(&opo->actions[n_actions], 
                                                data_len, data_len);
            ds_put_char(string, '\n');
            ds_put_cstr(string, packet);
            free(packet);
        }
    } else {
        ds_put_format(string, " buffer=0x%08"PRIx32, ntohl(opo->buffer_id));
    }
    ds_put_char(string, '\n');
}

/* qsort comparison function. */
static int
compare_ports(const void *a_, const void *b_)
{
    const struct ofp_phy_port *a = a_;
    const struct ofp_phy_port *b = b_;
    uint16_t ap = ntohs(a->port_no);
    uint16_t bp = ntohs(b->port_no);

    return ap < bp ? -1 : ap > bp;
}

static void
ofp_print_phy_port(struct ds *string, const struct ofp_phy_port *port)
{
    uint8_t name[OFP_MAX_PORT_NAME_LEN];
    int j;

    memcpy(name, port->name, sizeof name);
    for (j = 0; j < sizeof name - 1; j++) {
        if (!isprint(name[j])) {
            break;
        }
    }
    name[j] = '\0';

    ds_put_char(string, ' ');
    ofp_print_port_name(string, ntohs(port->port_no));
    ds_put_format(string, "(%s): addr:"ETH_ADDR_FMT", speed:%d, flags:%#x, "
            "feat:%#x\n", name, 
            ETH_ADDR_ARGS(port->hw_addr), ntohl(port->speed),
            ntohl(port->flags), ntohl(port->features));
}

/* Pretty-print the struct ofp_switch_features of 'len' bytes at 'oh' to
 * 'string' at the given 'verbosity' level. */
static void
ofp_print_switch_features(struct ds *string, const void *oh, size_t len,
                          int verbosity)
{
    const struct ofp_switch_features *osf = oh;
    struct ofp_phy_port port_list[OFPP_MAX];
    int n_ports;
    int i;

    ds_put_format(string, "dp id:%"PRIx64"\n", ntohll(osf->datapath_id));
    ds_put_format(string, "n_tables:%d, n_buffers:%d\n", osf->n_tables,
            ntohl(osf->n_buffers));
    ds_put_format(string, "features: capabilities:%#x, actions:%#x\n",
           ntohl(osf->capabilities), ntohl(osf->actions));

    if (ntohs(osf->header.length) >= sizeof *osf) {
        len = MIN(len, ntohs(osf->header.length));
    }
    n_ports = (len - sizeof *osf) / sizeof *osf->ports;

    memcpy(port_list, osf->ports, (len - sizeof *osf));
    qsort(port_list, n_ports, sizeof port_list[0], compare_ports);
    for (i = 0; i < n_ports; i++) {
        ofp_print_phy_port(string, &port_list[i]);
    }
}

/* Pretty-print the struct ofp_switch_config of 'len' bytes at 'oh' to 'string'
 * at the given 'verbosity' level. */
static void
ofp_print_switch_config(struct ds *string, const void *oh, size_t len,
                        int verbosity)
{
    const struct ofp_switch_config *osc = oh;
    uint16_t flags;

    flags = ntohs(osc->flags);
    if (flags & OFPC_SEND_FLOW_EXP) {
        flags &= ~OFPC_SEND_FLOW_EXP;
        ds_put_format(string, " (sending flow expirations)");
    }
    if (flags) {
        ds_put_format(string, " ***unknown flags 0x%04"PRIx16"***", flags);
    }

    ds_put_format(string, " miss_send_len=%"PRIu16"\n", ntohs(osc->miss_send_len));
}

static void print_wild(struct ds *string, const char *leader, int is_wild,
            int verbosity, const char *format, ...) 
            __attribute__((format(printf, 5, 6)));

static void print_wild(struct ds *string, const char *leader, int is_wild,
                       int verbosity, const char *format, ...) 
{
    if (is_wild && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (!is_wild) {
        va_list args;

        va_start(args, format);
        ds_put_format_valist(string, format, args);
        va_end(args);
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

static void
print_ip_netmask(struct ds *string, const char *leader, uint32_t ip,
                 uint32_t wild_bits, int verbosity)
{
    if (wild_bits >= 32 && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (wild_bits < 32) {
        ds_put_format(string, IP_FMT, IP_ARGS(&ip));
        if (wild_bits) {
            ds_put_format(string, "/%d", 32 - wild_bits);
        }
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

/* Pretty-print the ofp_match structure */
static void ofp_print_match(struct ds *f, const struct ofp_match *om, 
        int verbosity)
{
    uint32_t w = ntohl(om->wildcards);
    bool skip_type = false;
    bool skip_proto = false;

    if (!(w & OFPFW_DL_TYPE)) {
        skip_type = true;
        if (om->dl_type == htons(ETH_TYPE_IP)) {
            if (!(w & OFPFW_NW_PROTO)) {
                skip_proto = true;
                if (om->nw_proto == IP_TYPE_ICMP) {
                    ds_put_cstr(f, "icmp,");
                } else if (om->nw_proto == IP_TYPE_TCP) {
                    ds_put_cstr(f, "tcp,");
                } else if (om->nw_proto == IP_TYPE_UDP) {
                    ds_put_cstr(f, "udp,");
                } else {
                    ds_put_cstr(f, "ip,");
                    skip_proto = false;
                }
            } else {
                ds_put_cstr(f, "ip,");
            }
        } else if (om->dl_type == htons(ETH_TYPE_ARP)) {
            ds_put_cstr(f, "arp,");
        } else {
            skip_type = false;
        }
    }
    print_wild(f, "in_port=", w & OFPFW_IN_PORT, verbosity,
               "%d", ntohs(om->in_port));
    print_wild(f, "dl_vlan=", w & OFPFW_DL_VLAN, verbosity,
               "0x%04x", ntohs(om->dl_vlan));
    print_wild(f, "dl_src=", w & OFPFW_DL_SRC, verbosity,
               ETH_ADDR_FMT, ETH_ADDR_ARGS(om->dl_src));
    print_wild(f, "dl_dst=", w & OFPFW_DL_DST, verbosity,
               ETH_ADDR_FMT, ETH_ADDR_ARGS(om->dl_dst));
    if (!skip_type) {
        print_wild(f, "dl_type=", w & OFPFW_DL_TYPE, verbosity,
                   "0x%04x", ntohs(om->dl_type));
    }
    print_ip_netmask(f, "nw_src=", om->nw_src,
                     (w & OFPFW_NW_SRC_MASK) >> OFPFW_NW_SRC_SHIFT, verbosity);
    print_ip_netmask(f, "nw_dst=", om->nw_dst,
                     (w & OFPFW_NW_DST_MASK) >> OFPFW_NW_DST_SHIFT, verbosity);
    if (!skip_proto) {
        print_wild(f, "nw_proto=", w & OFPFW_NW_PROTO, verbosity,
                   "%u", om->nw_proto);
    }
    print_wild(f, "tp_src=", w & OFPFW_TP_SRC, verbosity,
               "%d", ntohs(om->tp_src));
    print_wild(f, "tp_dst=", w & OFPFW_TP_DST, verbosity,
               "%d", ntohs(om->tp_dst));
}

/* Pretty-print the OFPT_FLOW_MOD packet of 'len' bytes at 'oh' to 'string'
 * at the given 'verbosity' level. */
static void
ofp_print_flow_mod(struct ds *string, const void *oh, size_t len, 
                   int verbosity)
{
    const struct ofp_flow_mod *ofm = oh;

    ofp_print_match(string, &ofm->match, verbosity);
    ds_put_format(string, " cmd:%d idle:%d hard:%d pri:%d buf:%#x", 
            ntohs(ofm->command), ntohs(ofm->idle_timeout),
            ntohs(ofm->hard_timeout),
            ofm->match.wildcards ? ntohs(ofm->priority) : (uint16_t)-1,
            ntohl(ofm->buffer_id));
    ofp_print_actions(string, ofm->actions,
                      len - offsetof(struct ofp_flow_mod, actions));
    ds_put_char(string, '\n');
}

/* Pretty-print the OFPT_FLOW_EXPIRED packet of 'len' bytes at 'oh' to 'string'
 * at the given 'verbosity' level. */
static void
ofp_print_flow_expired(struct ds *string, const void *oh, size_t len, 
                       int verbosity)
{
    const struct ofp_flow_expired *ofe = oh;

    ofp_print_match(string, &ofe->match, verbosity);
    ds_put_cstr(string, " reason=");
    switch (ofe->reason) {
    case OFPER_IDLE_TIMEOUT:
        ds_put_cstr(string, "idle");
        break;
    case OFPER_HARD_TIMEOUT:
        ds_put_cstr(string, "hard");
        break;
    default:
        ds_put_format(string, "**%"PRIu8"**", ofe->reason);
        break;
    }
    ds_put_format(string, 
         " pri%"PRIu16" secs%"PRIu32" pkts%"PRIu64" bytes%"PRIu64"\n", 
         ofe->match.wildcards ? ntohs(ofe->priority) : (uint16_t)-1,
         ntohl(ofe->duration), ntohll(ofe->packet_count), 
         ntohll(ofe->byte_count));
}

struct error_type {
    int type;
    int code;
    const char *name;
};

static const struct error_type error_types[] = {
#define ERROR_TYPE(TYPE) {TYPE, -1, #TYPE}
#define ERROR_CODE(TYPE, CODE) {TYPE, CODE, #CODE}
    ERROR_TYPE(OFPET_HELLO_FAILED),
    ERROR_CODE(OFPET_HELLO_FAILED, OFPHFC_INCOMPATIBLE),

    ERROR_TYPE(OFPET_BAD_REQUEST),
    ERROR_CODE(OFPET_BAD_REQUEST, OFPBRC_BAD_VERSION),
    ERROR_CODE(OFPET_BAD_REQUEST, OFPBRC_BAD_TYPE),
    ERROR_CODE(OFPET_BAD_REQUEST, OFPBRC_BAD_STAT),
    ERROR_CODE(OFPET_BAD_REQUEST, OFPBRC_BAD_VERSION),
};
#define N_ERROR_TYPES ARRAY_SIZE(error_types)

static const char *
lookup_error_type(int type)
{
    const struct error_type *t;

    for (t = error_types; t < &error_types[N_ERROR_TYPES]; t++) {
        if (t->type == type && t->code == -1) {
            return t->name;
        }
    }
    return "?";
}

static const char *
lookup_error_code(int type, int code)
{
    const struct error_type *t;

    for (t = error_types; t < &error_types[N_ERROR_TYPES]; t++) {
        if (t->type == type && t->code == code) {
            return t->name;
        }
    }
    return "?";
}

/* Pretty-print the OFPT_ERROR packet of 'len' bytes at 'oh' to 'string'
 * at the given 'verbosity' level. */
static void
ofp_print_error_msg(struct ds *string, const void *oh, size_t len, 
                       int verbosity)
{
    const struct ofp_error_msg *oem = oh;
    int type = ntohs(oem->type);
    int code = ntohs(oem->code);
    char *s;

    ds_put_format(string, " type%d(%s) code%d(%s) payload:\n",
                  type, lookup_error_type(type),
                  code, lookup_error_code(type, code));

    switch (type) {
    case OFPET_HELLO_FAILED:
        ds_put_printable(string, (char *) oem->data, len - sizeof *oem);
        break;

    case OFPET_BAD_REQUEST:
        s = ofp_to_string(oem->data, len - sizeof *oem, 1);
        ds_put_cstr(string, s);
        free(s);
        break;

    default:
        ds_put_hex_dump(string, oem->data, len - sizeof *oem, 0, true);
        break;
    }
}

/* Pretty-print the OFPT_PORT_STATUS packet of 'len' bytes at 'oh' to 'string'
 * at the given 'verbosity' level. */
static void
ofp_print_port_status(struct ds *string, const void *oh, size_t len, 
                      int verbosity)
{
    const struct ofp_port_status *ops = oh;

    if (ops->reason == OFPPR_ADD) {
        ds_put_format(string, "add:");
    } else if (ops->reason == OFPPR_DELETE) {
        ds_put_format(string, "del:");
    } else if (ops->reason == OFPPR_MOD) {
        ds_put_format(string, "mod:");
    } else {
        ds_put_format(string, "err:");
    }

    ofp_print_phy_port(string, &ops->desc);
}

static void
ofp_desc_stats_reply(struct ds *string, const void *body, size_t len,
                     int verbosity)
{
    const struct ofp_desc_stats *ods = body;

    ds_put_format(string, "Manufacturer: %s\n", ods->mfr_desc);
    ds_put_format(string, "Hardware: %s\n", ods->hw_desc);
    ds_put_format(string, "Software: %s\n", ods->sw_desc);
    ds_put_format(string, "Serial Num: %s\n", ods->serial_num);
}

static void
ofp_flow_stats_request(struct ds *string, const void *oh, size_t len,
                      int verbosity) 
{
    const struct ofp_flow_stats_request *fsr = oh;

    if (fsr->table_id == 0xff) {
        ds_put_format(string, " table_id=any, ");
    } else {
        ds_put_format(string, " table_id=%"PRIu8", ", fsr->table_id);
    }

    ofp_print_match(string, &fsr->match, verbosity);
}

static void
ofp_flow_stats_reply(struct ds *string, const void *body_, size_t len,
                     int verbosity)
{
    const char *body = body_;
    const char *pos = body;
    for (;;) {
        const struct ofp_flow_stats *fs;
        ptrdiff_t bytes_left = body + len - pos;
        size_t length;

        if (bytes_left < sizeof *fs) {
            if (bytes_left != 0) {
                ds_put_format(string, " ***%td leftover bytes at end***",
                              bytes_left);
            }
            break;
        }

        fs = (const void *) pos;
        length = ntohs(fs->length);
        if (length < sizeof *fs) {
            ds_put_format(string, " ***length=%zu shorter than minimum %zu***",
                          length, sizeof *fs);
            break;
        } else if (length > bytes_left) {
            ds_put_format(string,
                          " ***length=%zu but only %td bytes left***",
                          length, bytes_left);
            break;
        } else if ((length - sizeof *fs) % sizeof fs->actions[0]) {
            ds_put_format(string,
                          " ***length=%zu has %zu bytes leftover in "
                          "final action***",
                          length,
                          (length - sizeof *fs) % sizeof fs->actions[0]);
            break;
        }

        ds_put_format(string, "  duration=%"PRIu32"s, ", ntohl(fs->duration));
        ds_put_format(string, "table_id=%"PRIu8", ", fs->table_id);
        ds_put_format(string, "priority=%"PRIu16", ", 
                    fs->match.wildcards ? ntohs(fs->priority) : (uint16_t)-1);
        ds_put_format(string, "n_packets=%"PRIu64", ",
                    ntohll(fs->packet_count));
        ds_put_format(string, "n_bytes=%"PRIu64", ", ntohll(fs->byte_count));
        ds_put_format(string, "idle_timeout=%"PRIu16",",
                      ntohs(fs->idle_timeout));
        ds_put_format(string, "hard_timeout=%"PRIu16",",
                      ntohs(fs->hard_timeout));
        ofp_print_match(string, &fs->match, verbosity);
        ofp_print_actions(string, fs->actions, length - sizeof *fs);
        ds_put_char(string, '\n');

        pos += length;
     }
}

static void
ofp_aggregate_stats_request(struct ds *string, const void *oh, size_t len,
                            int verbosity) 
{
    const struct ofp_aggregate_stats_request *asr = oh;

    if (asr->table_id == 0xff) {
        ds_put_format(string, " table_id=any, ");
    } else {
        ds_put_format(string, " table_id=%"PRIu8", ", asr->table_id);
    }

    ofp_print_match(string, &asr->match, verbosity);
}

static void
ofp_aggregate_stats_reply(struct ds *string, const void *body_, size_t len,
                          int verbosity)
{
    const struct ofp_aggregate_stats_reply *asr = body_;

    ds_put_format(string, " packet_count=%"PRIu64, ntohll(asr->packet_count));
    ds_put_format(string, " byte_count=%"PRIu64, ntohll(asr->byte_count));
    ds_put_format(string, " flow_count=%"PRIu32, ntohl(asr->flow_count));
}

static void print_port_stat(struct ds *string, const char *leader, 
                            uint64_t stat, int more)
{
    ds_put_cstr(string, leader);
    if (stat != -1) {
        ds_put_format(string, "%"PRIu64, stat);
    } else {
        ds_put_char(string, '?');
    }
    if (more) {
        ds_put_cstr(string, ", ");
    } else {
        ds_put_cstr(string, "\n");
    }
}

static void
ofp_port_stats_reply(struct ds *string, const void *body, size_t len,
                     int verbosity)
{
    const struct ofp_port_stats *ps = body;
    size_t n = len / sizeof *ps;
    ds_put_format(string, " %zu ports\n", n);
    if (verbosity < 1) {
        return;
    }

    for (; n--; ps++) {
        ds_put_format(string, "  port %2"PRIu16": ", ntohs(ps->port_no));

        ds_put_cstr(string, "rx ");
        print_port_stat(string, "pkts=", ntohll(ps->rx_packets), 1);
        print_port_stat(string, "bytes=", ntohll(ps->rx_bytes), 1);
        print_port_stat(string, "drop=", ntohll(ps->rx_dropped), 1);
        print_port_stat(string, "errs=", ntohll(ps->rx_errors), 1);
        print_port_stat(string, "frame=", ntohll(ps->rx_frame_err), 1);
        print_port_stat(string, "over=", ntohll(ps->rx_over_err), 1);
        print_port_stat(string, "crc=", ntohll(ps->rx_crc_err), 0);

        ds_put_cstr(string, "           tx ");
        print_port_stat(string, "pkts=", ntohll(ps->tx_packets), 1);
        print_port_stat(string, "bytes=", ntohll(ps->tx_bytes), 1);
        print_port_stat(string, "drop=", ntohll(ps->tx_dropped), 1);
        print_port_stat(string, "errs=", ntohll(ps->tx_errors), 1);
        print_port_stat(string, "coll=", ntohll(ps->collisions), 0);
    }
}

static void
ofp_table_stats_reply(struct ds *string, const void *body, size_t len,
                     int verbosity)
{
    const struct ofp_table_stats *ts = body;
    size_t n = len / sizeof *ts;
    ds_put_format(string, " %zu tables\n", n);
    if (verbosity < 1) {
        return;
    }

    for (; n--; ts++) {
        char name[OFP_MAX_TABLE_NAME_LEN + 1];
        strncpy(name, ts->name, sizeof name);
        name[OFP_MAX_TABLE_NAME_LEN] = '\0';

        ds_put_format(string, "  %d: %-8s: ", ts->table_id, name);
        ds_put_format(string, "wild=0x%05"PRIx32", ", ntohl(ts->wildcards));
        ds_put_format(string, "max=%6"PRIu32", ", ntohl(ts->max_entries));
        ds_put_format(string, "active=%6"PRIu32", ", ntohl(ts->active_count));
        ds_put_format(string, "matched=%6"PRIu64"\n",
                      ntohll(ts->matched_count));
     }
}

static void
vendor_stat(struct ds *string, const void *body, size_t len,
            int verbosity UNUSED)
{
    ds_put_format(string, " vendor=%08"PRIx32, ntohl(*(uint32_t *) body));
    ds_put_format(string, " %zu bytes additional data",
                  len - sizeof(uint32_t));
}

enum stats_direction {
    REQUEST,
    REPLY
};

static void
print_stats(struct ds *string, int type, const void *body, size_t body_len,
            int verbosity, enum stats_direction direction)
{
    struct stats_msg {
        size_t min_body, max_body;
        void (*printer)(struct ds *, const void *, size_t len, int verbosity);
    };

    struct stats_type {
        int type;
        const char *name;
        struct stats_msg request;
        struct stats_msg reply;
    };

    static const struct stats_type stats_types[] = {
        {
            OFPST_DESC,
            "description",
            { 0, 0, NULL },
            { 0, SIZE_MAX, ofp_desc_stats_reply },
        },
        {
            OFPST_FLOW,
            "flow",
            { sizeof(struct ofp_flow_stats_request),
              sizeof(struct ofp_flow_stats_request),
              ofp_flow_stats_request },
            { 0, SIZE_MAX, ofp_flow_stats_reply },
        },
        {
            OFPST_AGGREGATE,
            "aggregate",
            { sizeof(struct ofp_aggregate_stats_request),
              sizeof(struct ofp_aggregate_stats_request),
              ofp_aggregate_stats_request },
            { sizeof(struct ofp_aggregate_stats_reply),
              sizeof(struct ofp_aggregate_stats_reply),
              ofp_aggregate_stats_reply },
        },
        {
            OFPST_TABLE,
            "table",
            { 0, 0, NULL },
            { 0, SIZE_MAX, ofp_table_stats_reply },
        },
        {
            OFPST_PORT,
            "port",
            { 0, 0, NULL, },
            { 0, SIZE_MAX, ofp_port_stats_reply },
        },
        {
            OFPST_VENDOR,
            "vendor-specific",
            { sizeof(uint32_t), SIZE_MAX, vendor_stat },
            { sizeof(uint32_t), SIZE_MAX, vendor_stat },
        },
        {
            -1,
            "unknown",
            { 0, 0, NULL, },
            { 0, 0, NULL, },
        },
    };

    const struct stats_type *s;
    const struct stats_msg *m;

    if (type >= ARRAY_SIZE(stats_types) || !stats_types[type].name) {
        ds_put_format(string, " ***unknown type %d***", type);
        return;
    }
    for (s = stats_types; s->type >= 0; s++) {
        if (s->type == type) {
            break;
        }
    }
    ds_put_format(string, " type=%d(%s)\n", type, s->name);

    m = direction == REQUEST ? &s->request : &s->reply;
    if (body_len < m->min_body || body_len > m->max_body) {
        ds_put_format(string, " ***body_len=%zu not in %zu...%zu***",
                      body_len, m->min_body, m->max_body);
        return;
    }
    if (m->printer) {
        m->printer(string, body, body_len, verbosity);
    }
}

static void
ofp_stats_request(struct ds *string, const void *oh, size_t len, int verbosity)
{
    const struct ofp_stats_request *srq = oh;

    if (srq->flags) {
        ds_put_format(string, " ***unknown flags 0x%04"PRIx16"***",
                      ntohs(srq->flags));
    }

    print_stats(string, ntohs(srq->type), srq->body,
                len - offsetof(struct ofp_stats_request, body),
                verbosity, REQUEST);
}

static void
ofp_stats_reply(struct ds *string, const void *oh, size_t len, int verbosity)
{
    const struct ofp_stats_reply *srp = oh;

    ds_put_cstr(string, " flags=");
    if (!srp->flags) {
        ds_put_cstr(string, "none");
    } else {
        uint16_t flags = ntohs(srp->flags);
        if (flags & OFPSF_REPLY_MORE) {
            ds_put_cstr(string, "[more]");
            flags &= ~OFPSF_REPLY_MORE;
        }
        if (flags) {
            ds_put_format(string, "[***unknown flags 0x%04"PRIx16"***]", flags);
        }
    }

    print_stats(string, ntohs(srp->type), srp->body,
                len - offsetof(struct ofp_stats_reply, body),
                verbosity, REPLY);
}

static void
ofp_echo(struct ds *string, const void *oh, size_t len, int verbosity)
{
    const struct ofp_header *hdr = oh;

    ds_put_format(string, " %zu bytes of payload\n", len - sizeof *hdr);
    if (verbosity > 1) {
        ds_put_hex_dump(string, hdr, len - sizeof *hdr, 0, true); 
    }
}

struct openflow_packet {
    uint8_t type;
    const char *name;
    size_t min_size;
    void (*printer)(struct ds *, const void *, size_t len, int verbosity);
};

static const struct openflow_packet packets[] = {
    {
        OFPT_HELLO,
        "hello",
        sizeof (struct ofp_header),
        NULL,
    },
    {
        OFPT_FEATURES_REQUEST,
        "features_request",
        sizeof (struct ofp_header),
        NULL,
    },
    {
        OFPT_FEATURES_REPLY,
        "features_reply",
        sizeof (struct ofp_switch_features),
        ofp_print_switch_features,
    },
    {
        OFPT_GET_CONFIG_REQUEST,
        "get_config_request",
        sizeof (struct ofp_header),
        NULL,
    },
    {
        OFPT_GET_CONFIG_REPLY,
        "get_config_reply",
        sizeof (struct ofp_switch_config),
        ofp_print_switch_config,
    },
    {
        OFPT_SET_CONFIG,
        "set_config",
        sizeof (struct ofp_switch_config),
        ofp_print_switch_config,
    },
    {
        OFPT_PACKET_IN,
        "packet_in",
        offsetof(struct ofp_packet_in, data),
        ofp_packet_in,
    },
    {
        OFPT_PACKET_OUT,
        "packet_out",
        sizeof (struct ofp_packet_out),
        ofp_packet_out,
    },
    {
        OFPT_FLOW_MOD,
        "flow_mod",
        sizeof (struct ofp_flow_mod),
        ofp_print_flow_mod,
    },
    {
        OFPT_FLOW_EXPIRED,
        "flow_expired",
        sizeof (struct ofp_flow_expired),
        ofp_print_flow_expired,
    },
    {
        OFPT_PORT_MOD,
        "port_mod",
        sizeof (struct ofp_port_mod),
        NULL,
    },
    {
        OFPT_PORT_STATUS,
        "port_status",
        sizeof (struct ofp_port_status),
        ofp_print_port_status
    },
    {
        OFPT_ERROR,
        "error_msg",
        sizeof (struct ofp_error_msg),
        ofp_print_error_msg,
    },
    {
        OFPT_STATS_REQUEST,
        "stats_request",
        sizeof (struct ofp_stats_request),
        ofp_stats_request,
    },
    {
        OFPT_STATS_REPLY,
        "stats_reply",
        sizeof (struct ofp_stats_reply),
        ofp_stats_reply,
    },
    {
        OFPT_ECHO_REQUEST,
        "echo_request",
        sizeof (struct ofp_header),
        ofp_echo,
    },
    {
        OFPT_ECHO_REPLY,
        "echo_reply",
        sizeof (struct ofp_header),
        ofp_echo,
    },
};

/* Composes and returns a string representing the OpenFlow packet of 'len'
 * bytes at 'oh' at the given 'verbosity' level.  0 is a minimal amount of
 * verbosity and higher numbers increase verbosity.  The caller is responsible
 * for freeing the string. */
char *
ofp_to_string(const void *oh_, size_t len, int verbosity)
{
    struct ds string = DS_EMPTY_INITIALIZER;
    const struct ofp_header *oh = oh_;
    const struct openflow_packet *pkt;

    if (len < sizeof(struct ofp_header)) {
        ds_put_cstr(&string, "OpenFlow packet too short:\n");
        ds_put_hex_dump(&string, oh, len, 0, true);
        return ds_cstr(&string);
    } else if (oh->version != OFP_VERSION) {
        ds_put_format(&string, "Bad OpenFlow version %"PRIu8":\n", oh->version);
        ds_put_hex_dump(&string, oh, len, 0, true);
        return ds_cstr(&string);
    }

    for (pkt = packets; ; pkt++) {
        if (pkt >= &packets[ARRAY_SIZE(packets)]) {
            ds_put_format(&string, "Unknown OpenFlow packet type %"PRIu8":\n",
                          oh->type);
            ds_put_hex_dump(&string, oh, len, 0, true);
            return ds_cstr(&string);
        } else if (oh->type == pkt->type) {
            break;
        }
    }

    ds_put_format(&string, "%s (xid=0x%"PRIx32"):", pkt->name, oh->xid);

    if (ntohs(oh->length) > len)
        ds_put_format(&string, " (***truncated to %zu bytes from %"PRIu16"***)",
                len, ntohs(oh->length));
    else if (ntohs(oh->length) < len) {
        ds_put_format(&string, " (***only uses %"PRIu16" bytes out of %zu***)\n",
                ntohs(oh->length), len);
        len = ntohs(oh->length);
    }

    if (len < pkt->min_size) {
        ds_put_format(&string, " (***length=%zu < min_size=%zu***)\n",
                len, pkt->min_size);
    } else if (!pkt->printer) {
        if (len > sizeof *oh) {
            ds_put_format(&string, " length=%"PRIu16" (decoder not implemented)\n",
                          ntohs(oh->length)); 
        }
    } else {
        pkt->printer(&string, oh, len, verbosity);
    }
    if (verbosity >= 3) {
        ds_put_hex_dump(&string, oh, len, 0, true);
    }
    if (string.string[string.length - 1] != '\n') {
        ds_put_char(&string, '\n');
    }
    return ds_cstr(&string);
}

static void
print_and_free(FILE *stream, char *string) 
{
    fputs(string, stream);
    free(string);
}

/* Pretty-print the OpenFlow packet of 'len' bytes at 'oh' to 'stream' at the
 * given 'verbosity' level.  0 is a minimal amount of verbosity and higher
 * numbers increase verbosity. */
void
ofp_print(FILE *stream, const void *oh, size_t len, int verbosity)
{
    print_and_free(stream, ofp_to_string(oh, len, verbosity));
}

/* Dumps the contents of the Ethernet frame in the 'len' bytes starting at
 * 'data' to 'stream' using tcpdump.  'total_len' specifies the full length of
 * the Ethernet frame (of which 'len' bytes were captured).
 *
 * This starts and kills a tcpdump subprocess so it's quite expensive. */
void
ofp_print_packet(FILE *stream, const void *data, size_t len, size_t total_len)
{
    print_and_free(stream, ofp_packet_to_string(data, len, total_len));
}
