# How to Run


# Building Lotus

cmake


Docker

# Compiling Programs to LLVM bitcode

- clang
- wllvm, gllvm
- clang with LTO of LLVM
- Generate_Linux_Kernel_Bitcode: https://github.com/ZHYfeng/Generate_Linux_Kernel_Bitcode


## clang 
A simple example
~~~~
void swap(char **p, char **q){
  char* t = *p; 
       *p = *q; 
       *q = t;
}
int main(){
      char a1, b1; 
      char *a = &a1;
      char *b = &b1;
      swap(&a,&b);
}
~~~~

~~~~
clang -S -c -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm swap.c -o swap.ll
opt -S -p=mem2reg swap.ll -o swap.ll
~~~~

## wllvm

### Option 1. Install gllvm
Create a ubuntu docker first. 

Install the go tool 1.16 and then install gllvm

* `wget https://go.dev/dl/go1.16.15.linux-amd64.tar.gz`

* `rm -rf /usr/local/go && tar -C /usr/local -xzf go1.16.15.linux-amd64.tar.gz`

* `export PATH=$PATH:/usr/local/go/bin`

* `go version`

* `GO111MODULE=off go get github.com/SRI-CSL/gllvm/cmd/...`

After this, we have the gllvm commands including `gclang`, `gclang++`, `get-bc`, `gsanity-check` under the `/root/go/bin/`

### check env

Run `export LLVM_COMPILER_PATH=$SVF_INSTALL_LLVM_14/bin` to set the llvm path to LLVM14 that SVF installs and run `gsanity-check` to confirm the enviroment.

### Option 2. Install wllvm
`pip install wllvm`
After this, we have the wllvm commands including `wllvm`, `wllvm++`, `extract-bc`, `wllvm-sanity-checker`

#### check env
Run `export LLVM_COMPILER=clang` to set the compiler. If we need a deferent version of LLVM, run `export LLVM_CC_NAME=clang-9`, `export LLVM_CXX_NAME=clang-9++`, `export LLVM_LINK_NAME=llvm-link-9`, `export LLVM_AR_NAME=llvm-ar-9` for example the version is llvm-9.

Finally, run `wllvm-sanity-checker` to confirm the enviroment.

### 3. compile bc

When compiling a source code, we need to set `CC=gclang` or `CXX=gclang++`. For example, `CC=gclang make`
For wllvm, change to `CC=wllvm` or `CXX=wllvm++`

#### 4. RIOT example https://github.com/RIOT-OS/RIOT

`git clone https://github.com/RIOT-OS/RIOT`

`cd RIOT/examples/default/`

`make CC="gclang -O0 -Xclang -disable-O0-optnone -g -save-temps=obj -fno-discard-value-names -w" CXX="gclang++ -O0 -Xclang -disable-O0-optnone -g -save-temps=obj -fno-discard-value-names -w" -j8`

`cd bin/native/`

`get-bc default.elf` we get the default.elf.bc at this step.

`llvm-dis default.elf.bc` if readable ll is needed.


Other resources: https://github.com/mbarbar/crux-bitcode