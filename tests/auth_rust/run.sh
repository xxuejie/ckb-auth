cd ../..
make -f examples/auth-demo/Makefile all
cd tests/auth_rust
cargo test
