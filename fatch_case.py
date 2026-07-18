#!/usr/bin/env python3
import json, sys, os, urllib.request

def rpc(url, method, params):
    req = urllib.request.Request(url, data=json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        body = json.load(resp)
    if "result" not in body or body["result"] is None:
        raise SystemExit(f"RPC {method} failed: {body.get('error', 'null result')}")
    return body["result"]

def write_case(outdir, tx, receipt, wallet):
    os.makedirs(outdir, exist_ok=True)
    json.dump(tx, open(os.path.join(outdir, "tx.json"), "w"), indent=2)
    json.dump(receipt, open(os.path.join(outdir, "receipt.json"), "w"), indent=2)
    open(os.path.join(outdir, "wallet.txt"), "w").write(wallet.lower())
    exp = os.path.join(outdir, "expected.json")
    if not os.path.exists(exp):
        json.dump({"_TODO": "fill by hand after checking the explorer; remove keys you don't want asserted",
                   "valid": True, "isSwap": True, "isBuy": True,
                   "tokenAddr": "0x_FILL_ME", "venue": "OPTIONAL_OR_DELETE"},
                  open(exp, "w"), indent=2)
    print(f"case written: {outdir}")
    print("next: edit expected.json by hand, then run run_tests")

def main():
    args = sys.argv[1:]
    if len(args) == 4 and args[0] == "--rpc":
        _, url, txhash, wallet = args
        name = txhash[2:10]
        tx = rpc(url, "eth_getTransactionByHash", [txhash])
        receipt = rpc(url, "eth_getTransactionReceipt", [txhash])
        write_case(os.path.join(os.path.dirname(os.path.abspath(__file__)), name), tx, receipt, wallet)
    elif len(args) == 4 and args[0] == "--files":
        _, txfile, rcfile, wallet = args
        tx = json.load(open(txfile)); receipt = json.load(open(rcfile))
        name = (tx.get("hash") or "case")[2:10] or "case"
        write_case(os.path.join(os.path.dirname(os.path.abspath(__file__)), name), tx, receipt, wallet)
    else:
        print("usage:\n  fetch_case.py --rpc <rpc_url> <tx_hash> <wallet>\n"
              "  fetch_case.py --files <tx.json> <receipt.json> <wallet>")
        sys.exit(1)

if __name__ == "__main__":
    main()
