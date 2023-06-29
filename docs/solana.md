# ckb-auth solana interoperability
Ckb-auth library is able to verify a lot of blockchain signatures including the solana.
We can use `solana` (or other compatible wallet) to generate valid signatures and validate them on-chain with ckb-auth.

A simple way to use solana signature algorithm to lock ckb cells
is to sign the transaction hash (or maybe `sighash_all`, i.e. hashing all fields 
including transaction hash and other witnesses in this input group)
with `solana-cli`, and then leverage ckb-auth to check the validity of this signature.
See [the docs](./auth.md) for more details.

# Signing a transaction with solana-cli

## Downloading the solana binaries
The commands below download the official `solana` binaries into the directory `/usr/local`.

```bash
tarball=solana-release-x86_64-unknown-linux-gnu.tar.bz2
wget -O "$tarball" https://github.com/solana-labs/solana/releases/download/v1.16.1/solana-release-x86_64-unknown-linux-gnu.tar.bz2
tar xvaf "$tarball"
sudo cp -r solana-*/* /usr/local/
solana --help
```


## Creating a new account or use the existing solana account
See also [Keypairs and Wallets | Solana Cookbook](https://solanacookbook.com/references/keypairs-and-wallets.html#how-to-generate-a-new-keypair)
```
solana-keygen new -o keypair.json # Create a new keypair
solana-keygen recover -o keypair.json # Recover old keypair from seed phrase
```

## Obtaining public key hash needed by ckb-auth
The public key hash is the first 20 bits of the public key hashed by `blake2b_256`.
We can obtain the public key by running.
```bash
solana-keygen pubkey keypair.json
```
The result of this command is base58-encoded (like `JA6jjaAha7SNVryoecCcNTH7vvqwhX8nBDiyAfeP1FcV`).
You may [decode it online](http://lenschulwitz.com/base58) or run
the command `base58 -d <<< JA6jjaAha7SNVryoecCcNTH7vvqwhX8nBDiyAfeP1FcV | xxd` 
([keis/base58](https://github.com/keis/base58) and [xxd(1)](https://linux.die.net/man/1/xxd)required).

## Signing a message with solana cli
To sign the message `42562dba645ed5a35a4dd6ec27ebc2c4c6a52adc27cc18c924c32f474018482c`, we need to first obtain
the base58 encoding of the message, which is `5Tx8F3jgSHx21CbtjwmdaKPLM5tWmreWAnPrbqHomSJF` and then run the command
```bash
solana transfer --from keypair.json --blockhash 5Tx8F3jgSHx21CbtjwmdaKPLM5tWmreWAnPrbqHomSJF 6dN24Y1wBW66CxLfXbRT9umy1PMed8ZmfMWsghopczFg 0 --output json --verbose --dump-transaction-message --sign-only
```
Here we construct a valid transaction with the message embedded as blockhash in the transcaction.
When this trancation is signed by our private key, ckb-auth will recognize it as a valid transaction.
Here the command line argument `6dN24Y1wBW66CxLfXbRT9umy1PMed8ZmfMWsghopczFg` represents the receiver account,
and `0` represents the transaction amount. ckb-auth does not really care about the receiver account and amount,
as long as the signature is valid and the solana message contains the message we intend to sign as blockhash.
The output of the above command is
```json
{
  "blockhash": "5Tx8F3jgSHx21CbtjwmdaKPLM5tWmreWAnPrbqHomSJF",
  "message": "AgABBJSNMEbMgjVcD2BXPvhOXxJU05GvhaDy1fk4eHjaxqwi/utkQqq8LIfr9fI22x+/1HP8IUQtr3uq8nk4FejyvYJTmxv1p8BT0M957h8yk9jMKdP9h5TCYtl0IFgKC5YD/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQlYtumRe1aNaTdbsJ+vCxMalKtwnzBjJJMMvR0AYSCwBAwIBAgwCAAAAAAAAAAAAAAA=",
  "signers": [
    "JA6jjaAha7SNVryoecCcNTH7vvqwhX8nBDiyAfeP1FcV=4QkrErPeLZKrrHcrEsh67AFXZ818544wz9QBWAjRm5pcvU8LJFChi4FkTjSRrf4LmonLJqbPfXdM17z2JCJ65bLg"
  ]
}
```

Here the base64 message is a compact representation of the solana transaction to be signed by
`JA6jjaAha7SNVryoecCcNTH7vvqwhX8nBDiyAfeP1FcV`, and it contains the blockhash we passed.
The signers fields include the base58-encoded public key and the signature. The public key and
the signature are separated by an equality sign (`=`). Below script shows the contents of
public key and signature.

```bash
for i in $(solana transfer --blockhash 5Tx8F3jgSHx21CbtjwmdaKPLM5tWmreWAnPrbqHomSJF keypair.json 0 --output json --verbose --dump-transaction-message --sign-only | jq -r '.signers[]' | tr '=' '\n'); do base58 -d <<< "$i" | xxd; echo; done
```

A sample output is
```
00000000: feeb 6442 aabc 2c87 ebf5 f236 db1f bfd4  ..dB..,....6....
00000010: 73fc 2144 2daf 7baa f279 3815 e8f2 bd82  s.!D-.{..y8.....

00000000: 5757 23e2 4d9f 62bd 516b f33b 3ef0 43d0  WW#.M.b.Qk.;>.C.
00000010: 52b1 474b 6edb 5534 7042 0788 7737 3091  R.GKn.U4pB..w70.
00000020: 0471 a5dc 1203 8a05 483b 50b2 24b0 f394  .q......H;P.$...
00000030: 8900 c0be 9fe3 9cf3 5b3a ff0a 686e c60f  ........[:..hn..
```

## Required information for ckb-auth to verify the validity of solana signatures
Ckb-auth requires the signature, public key and message to verify the validity of the signature.
The signature and public key are contained in the signers of `solana transfer` output,
and the message correponds to the field message of `solana transfer` output.
