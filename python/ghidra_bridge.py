#!/usr/bin/env python3
"""Start the GhidraMCP bridge (bethington/ghidra-mcp, 7.x) for ONE specific Ghidra.

The bridge is the MCP server agent.py talks to over stdio; it forwards tool
calls to the GhidraMCP plugin inside a running Ghidra. Left to itself the
bridge picks its Ghidra like this:

  1. any Ghidra on this Mac that it finds through a unix socket, then
  2. the URL in $GHIDRA_MCP_URL.

So with a Ghidra open on the Mac, a session meant for the remote Ghidra would
silently use the local one. `--tcp-only` switches step 1 off, which makes
`--url` the only Ghidra this bridge can reach.

    ghidra_bridge.py --bridge-dir ~/Documents/ghidra-mcp --url http://127.0.0.1:18089 --tcp-only

Anything after `--` is passed to the bridge unchanged (e.g. `-- --no-lazy`).
"""
from __future__ import annotations

import argparse
import os
import sys


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bridge-dir", required=True, help="checkout of bethington/ghidra-mcp")
    p.add_argument("--url", required=True, help="the plugin's address, e.g. http://127.0.0.1:8089")
    p.add_argument("--tcp-only", action="store_true",
                   help="ignore Ghidra instances found through local unix sockets")
    p.add_argument("bridge_args", nargs="*", help="extra arguments for the bridge (after --)")
    a = p.parse_args()

    package_parent = os.path.join(os.path.expanduser(a.bridge_dir), "python")
    if not os.path.isdir(os.path.join(package_parent, "bridge_mcp_ghidra")):
        print(f"ghidra_bridge: no bridge_mcp_ghidra package under {package_parent}. "
              f"The checkout must be ghidra-mcp 7.x (the same release as the plugin in Ghidra).",
              file=sys.stderr)
        return 2
    sys.path.insert(0, package_parent)
    os.environ["GHIDRA_MCP_URL"] = a.url

    from bridge_mcp_ghidra import cli, discovery   # noqa: E402 - needs sys.path set first

    if a.tcp_only:
        if not hasattr(discovery, "discover_instances"):
            print("ghidra_bridge: this bridge version has no discovery.discover_instances; "
                  "--tcp-only can't be applied. Update python/ghidra_bridge.py.", file=sys.stderr)
            return 2
        discovery.discover_instances = lambda: []
    print(f"ghidra_bridge: using Ghidra at {a.url}"
          f"{' only (local unix-socket instances ignored)' if a.tcp_only else ', or a local instance if one is found'}",
          file=sys.stderr, flush=True)

    sys.argv = ["bridge_mcp_ghidra", "--transport", "stdio", *a.bridge_args]
    cli.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
