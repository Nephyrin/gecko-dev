
#include <stdio.h>

typedef unsigned nsresult;
typedef unsigned uint32;
typedef uint32   nsXPCVariant;
typedef uint32   PRUint32;


#if defined(WIN32)
#define NS_IMETHOD virtual nsresult __stdcall
#define NS_IMETHODIMP nsresult __stdcall
#else
#define NS_IMETHOD virtual nsresult
#define NS_IMETHODIMP nsresult
#endif


class base{
public:
  NS_IMETHOD ignored() = 0;
};

class foo : public base {
public:
  NS_IMETHOD callme1(int i, int j) = 0;
  NS_IMETHOD callme2(int i, int j) = 0;
  NS_IMETHOD callme3(int i, int j) = 0;
};

class bar : public foo{
public:
  NS_IMETHOD ignored();
  NS_IMETHOD callme1(int i, int j);
  NS_IMETHOD callme2(int i, int j);
  NS_IMETHOD callme3(int i, int j);
};

class baz : public base {
public:
  NS_IMETHOD ignored();
  NS_IMETHOD callme1();
  NS_IMETHOD callme2();
  NS_IMETHOD callme3();
  void setfoo(foo* f) {other = f;}

  foo* other;
};
NS_IMETHODIMP baz::ignored(){return 0;}

NS_IMETHODIMP bar::ignored(){return 0;}

NS_IMETHODIMP bar::callme1(int i, int j)
{
  printf("called bar::callme1 with: %d %d\n", i, j);
  return 5;
}

NS_IMETHODIMP bar::callme2(int i, int j)
{
  printf("called bar::callme2 with: %d %d\n", i, j);
  return 5;
}

NS_IMETHODIMP bar::callme3(int i, int j)
{
  printf("called bar::callme3 with: %d %d\n", i, j);
  return 5;
}

void docall(foo* f, int i, int j){
  f->callme1(i, j); 
}

/***************************************************************************/
#if defined(WIN32)

static int __stdcall
PrepareAndDispatch(baz* self, uint32 methodIndex,
                   uint32* args, uint32* stackBytesToPop)
{
    foo* a = self->other;
    int p1 = (int) *args;
    int p2 = (int) *(args+1);
    switch(methodIndex)
    {
    case 1: a->callme1(p1, p2); break;
    case 2: a->callme2(p1, p2); break;
    case 3: a->callme3(p1, p2); break;
    }
    *stackBytesToPop = 2*4;
    return 1;
}

static __declspec(naked) void SharedStub(void)
{
    __asm {
        push ebp            // set up simple stack frame
        mov  ebp, esp       // stack has: ebp/vtbl_index/retaddr/this/args
        push ecx            // make room for a ptr
        lea  eax, [ebp-4]   // pointer to stackBytesToPop
        push eax
        lea  ecx, [ebp+16]  // pointer to args
        push ecx
        mov  edx, [ebp+4]   // vtbl_index
        push edx
        mov  eax, [ebp+12]  // this
        push eax
        call PrepareAndDispatch
        mov  edx, [ebp+8]   // return address
        mov  ecx, [ebp-4]   // stackBytesToPop
        add  ecx, 12        // for this, the index, and ret address
        mov  esp, ebp
        pop  ebp
        add  esp, ecx       // fix up stack pointer
        jmp  edx            // simulate __stdcall return
    }
}

// these macros get expanded (many times) in the file #included below
#define STUB_ENTRY(n) \
__declspec(naked) nsresult __stdcall baz::callme##n() \
{ __asm push n __asm jmp SharedStub }


#else
/***************************************************************************/
// just Linux_x86 now. Add other later...

static int
PrepareAndDispatch(baz* self, uint32 methodIndex, uint32* args)
{
    foo* a = self->other;
    int p1 = (int) *args;
    int p2 = (int) *(args+1);
    switch(methodIndex)
    {
    case 1: a->callme1(p1, p2); break;
    case 2: a->callme2(p1, p2); break;
    case 3: a->callme3(p1, p2); break;
    }
    return 1;
}

//nsresult nsXPCWrappedJS::Stub##n() \

#define STUB_ENTRY(n) \
nsresult baz::callme##n() \
{ \
  register void* method = PrepareAndDispatch; \
  register nsresult result; \
  __asm__ __volatile__( \
    "leal   0x0c(%%ebp), %%ecx\n\t"    /* args */ \
    "pushl  %%ecx\n\t" \
    "pushl  $"#n"\n\t"                 /* method index */ \
    "movl   0x08(%%ebp), %%ecx\n\t"    /* this */ \
    "pushl  %%ecx\n\t" \
    "call   *%%edx"                    /* PrepareAndDispatch */ \
    : "=a" (result)     /* %0 */ \
    : "d" (method)      /* %1 */ \
    : "ax", "dx", "cx", "memory" ); \
    return result; \
}

#endif
/***************************************************************************/

STUB_ENTRY(1)
STUB_ENTRY(2)
STUB_ENTRY(3)

int main()
{
  foo* a = new bar();
  baz* b = new baz();
  
  foo* c = (foo*)b;

  b->setfoo(a);
  c->callme1(1,2);
  c->callme2(2,4);
  c->callme3(3,6);

  return 0;
}
