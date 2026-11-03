//! Which addresses eye data may go to. Every destination (the main output, the pupils sent straight to VRChat in
//! LiveLink mode and each extra output) is checked here each time its host is looked up.
//!
//! The home network table: 127/8, 10/8, 172.16/12, 192.168/16 and 169.254/16; ::1, fc00::/7 and fe80::/10; an
//! IPv4-mapped IPv6 address counts as its IPv4 part. 100.64/10 (carrier-grade NAT, which VPNs such as Tailscale use) is
//! allowed and shown as a VPN. Never: multicast (224/4, ff00::/8), the broadcast 255.255.255.255, and 0.0.0.0/8 or ::
//! (nothing to send to). Anything else is a public address: refused while lan_only is on, allowed and shown as public
//! while it is off. A subnet's own broadcast address (192.168.0.255, say) needs no check here: sockets never set
//! SO_BROADCAST, so the system refuses to send there (EACCES, "broadcast refused").

use serde::Serialize;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};

/// What kind of address a destination is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Net {
    /// In the home network table.
    Lan,
    /// 100.64/10: allowed, and shown as a VPN.
    Vpn,
    /// Outside the table: refused while lan_only is on.
    Public,
    /// Never sent to (multicast, broadcast, unspecified).
    Refused,
}

impl Net {
    /// Whether eye data may go to an address of this kind.
    pub fn allowed(self, lan_only: bool) -> bool {
        match self {
            Net::Lan | Net::Vpn => true,
            Net::Public => !lan_only,
            Net::Refused => false,
        }
    }
}

/// The kind of address `ip` is.
pub fn classify(ip: IpAddr) -> Net {
    match ip {
        IpAddr::V4(ip) => classify_v4(ip),
        IpAddr::V6(ip) => match ip.to_ipv4_mapped() {
            Some(v4) => classify_v4(v4),
            None => classify_v6(ip),
        },
    }
}

fn classify_v4(ip: Ipv4Addr) -> Net {
    let [a, b, ..] = ip.octets();
    if ip.is_multicast() || ip.is_broadcast() || a == 0 {
        Net::Refused
    } else if ip.is_loopback() || ip.is_private() || ip.is_link_local() {
        Net::Lan
    } else if a == 100 && (64..128).contains(&b) {
        Net::Vpn
    } else {
        Net::Public
    }
}

fn classify_v6(ip: Ipv6Addr) -> Net {
    let first = ip.segments()[0];
    if ip.is_multicast() || ip.is_unspecified() {
        Net::Refused
    } else if ip.is_loopback() || first & 0xfe00 == 0xfc00 || first & 0xffc0 == 0xfe80 {
        Net::Lan
    } else {
        Net::Public
    }
}

/// Why an address is not sent to: a short reason, as the panel shows it.
pub fn refusal(ip: IpAddr) -> &'static str {
    let ip = match ip {
        IpAddr::V6(v6) => v6.to_ipv4_mapped().map_or(ip, IpAddr::V4),
        v4 => v4,
    };
    match ip {
        IpAddr::V4(v4) if v4.is_broadcast() => "broadcast refused",
        _ if ip.is_multicast() => "multicast refused",
        _ if classify(ip) == Net::Refused => "not an address to send to",
        _ => "public address",
    }
}

/// The address to send to among those a host name was looked up as: the first allowed one in the order looked up (as
/// the main output took the first one before there was a gate), with its kind. When none is allowed, the reason the
/// first of them is refused; None for no address at all.
pub fn pick(
    addrs: impl IntoIterator<Item = SocketAddr>,
    lan_only: bool,
) -> Result<(SocketAddr, Net), Option<&'static str>> {
    let addrs: Vec<SocketAddr> = addrs.into_iter().collect();
    for addr in &addrs {
        let net = classify(addr.ip());
        if net.allowed(lan_only) {
            return Ok((*addr, net));
        }
    }
    Err(addrs.first().map(|addr| refusal(addr.ip())))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn net(text: &str) -> Net {
        classify(text.parse().unwrap())
    }

    #[test]
    fn ipv4_addresses_are_told_apart_by_the_table() {
        for ip in [
            "127.0.0.1",
            "127.255.255.254",
            "10.0.0.1",
            "10.255.255.255",
            "172.16.0.1",
            "172.31.255.254",
            "192.168.0.10",
            "192.168.255.255",
            "169.254.1.1",
        ] {
            assert_eq!(net(ip), Net::Lan, "{ip}");
        }
        for ip in ["100.64.0.1", "100.100.100.100", "100.127.255.254"] {
            assert_eq!(net(ip), Net::Vpn, "{ip}");
        }
        for ip in [
            "8.8.8.8",
            "1.1.1.1",
            "172.15.255.255",
            "172.32.0.1",
            "192.169.0.1",
            "100.63.255.255",
            "100.128.0.1",
            "11.0.0.1",
            "169.255.0.1",
            "192.0.2.1",
            "240.0.0.1",
        ] {
            assert_eq!(net(ip), Net::Public, "{ip}");
        }
        for ip in ["0.0.0.0", "0.1.2.3", "224.0.0.1", "239.255.255.250", "255.255.255.255"] {
            assert_eq!(net(ip), Net::Refused, "{ip}");
        }
    }

    #[test]
    fn ipv6_addresses_are_told_apart_by_the_table() {
        for ip in ["::1", "fc00::1", "fd12:3456:789a::1", "fe80::1", "febf::1"] {
            assert_eq!(net(ip), Net::Lan, "{ip}");
        }
        for ip in ["2001:db8::1", "2606:4700::1111", "fec0::1", "fe00::1", "64:ff9b::808:808", "::2"] {
            assert_eq!(net(ip), Net::Public, "{ip}");
        }
        for ip in ["::", "ff02::1", "ff05::fb"] {
            assert_eq!(net(ip), Net::Refused, "{ip}");
        }
    }

    #[test]
    fn mapped_ipv4_counts_as_its_ipv4_part() {
        assert_eq!(net("::ffff:192.168.0.10"), Net::Lan);
        assert_eq!(net("::ffff:127.0.0.1"), Net::Lan);
        assert_eq!(net("::ffff:100.64.0.1"), Net::Vpn);
        assert_eq!(net("::ffff:8.8.8.8"), Net::Public);
        assert_eq!(net("::ffff:255.255.255.255"), Net::Refused);
        assert_eq!(net("::ffff:224.0.0.1"), Net::Refused);
        assert_eq!(refusal("::ffff:255.255.255.255".parse().unwrap()), "broadcast refused");
    }

    #[test]
    fn lan_only_refuses_public_addresses_and_never_allows_the_refused_ones() {
        for lan_only in [true, false] {
            assert!(Net::Lan.allowed(lan_only) && Net::Vpn.allowed(lan_only));
            assert!(!Net::Refused.allowed(lan_only));
            assert_eq!(Net::Public.allowed(lan_only), !lan_only);
        }
        let reason = |ip: &str| refusal(ip.parse().unwrap());
        assert_eq!(reason("8.8.8.8"), "public address");
        assert_eq!(reason("255.255.255.255"), "broadcast refused");
        assert_eq!(reason("239.1.2.3"), "multicast refused");
        assert_eq!(reason("ff02::1"), "multicast refused");
        assert_eq!(reason("0.0.0.0"), "not an address to send to");
        assert_eq!(reason("::"), "not an address to send to");
        let names = serde_json::to_value([Net::Lan, Net::Vpn, Net::Public]).unwrap();
        assert_eq!(names, serde_json::json!(["lan", "vpn", "public"]));
    }

    #[test]
    fn a_host_with_several_addresses_uses_the_first_allowed() {
        let addrs = |list: &[&str]| -> Vec<SocketAddr> { list.iter().map(|text| text.parse().unwrap()).collect() };
        let picked = |list: &[&str], lan_only| pick(addrs(list), lan_only);
        let at = |text: &str| text.parse::<SocketAddr>().unwrap();
        // In the order they were looked up in (as before there was a gate), whatever the family
        assert_eq!(picked(&["[fd00::5]:9000", "192.168.0.5:9000"], true), Ok((at("[fd00::5]:9000"), Net::Lan)));
        assert_eq!(picked(&["192.168.0.5:9000", "[fd00::5]:9000"], true), Ok((at("192.168.0.5:9000"), Net::Lan)));
        assert_eq!(picked(&["10.0.0.2:1", "10.0.0.1:1"], true), Ok((at("10.0.0.2:1"), Net::Lan)));
        // A public one is skipped for an allowed one, even an IPv6 one after it
        assert_eq!(picked(&["8.8.8.8:9000", "[fd00::5]:9000"], true), Ok((at("[fd00::5]:9000"), Net::Lan)));
        assert_eq!(picked(&["8.8.8.8:9000", "100.64.1.2:9000"], true), Ok((at("100.64.1.2:9000"), Net::Vpn)));
        // Only public ones: refused while lan_only is on, the first one otherwise
        assert_eq!(picked(&["[2001:db8::1]:9000", "8.8.8.8:9000"], true), Err(Some("public address")));
        assert_eq!(picked(&["[2001:db8::1]:9000", "8.8.8.8:9000"], false), Ok((at("[2001:db8::1]:9000"), Net::Public)));
        // Never multicast, whatever lan_only says
        assert_eq!(picked(&["239.1.2.3:9000"], false), Err(Some("multicast refused")));
        assert_eq!(picked(&["239.1.2.3:9000", "8.8.8.8:9000"], false), Ok((at("8.8.8.8:9000"), Net::Public)));
        assert_eq!(picked(&[], true), Err(None));
    }
}
