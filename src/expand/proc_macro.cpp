/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/proc_macro.cpp
 * - Support for the `#[proc_macro_derive]` attribute
 */
#include <synext.hpp>
#include "../common.hpp"
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <hir/hir.hpp>  // ABI_RUST
#include "proc_macro.hpp"
#include <parse/lex.hpp>
#ifdef WIN32
#else
# include <unistd.h>    // read/write/pipe
# include <spawn.h>
# include <sys/wait.h>
#endif

#define NEWNODE(_ty, ...)   ::AST::ExprNodeP(new ::AST::ExprNode##_ty(__VA_ARGS__))

class Decorator_ProcMacroDerive:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::MetaItem& attr, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item& i) const override
    {
        if( i.is_None() )
            return;

        if( !i.is_Function() )
            TODO(sp, "Error for proc_macro_derive on non-Function");
        //auto& fcn = i.as_Function();
        auto trait_name = attr.items().at(0).name();
        ::std::vector<::std::string>    attributes;
        for(size_t i = 1; i < attr.items().size(); i ++)
        {
            if( attr.items()[i].name() == "attributes") {
                for(const auto& si : attr.items()[i].items()) {
                    attributes.push_back( si.name() );
                }
            }
        }

        // TODO: Store attributes for later use.
        crate.m_proc_macros.push_back(::std::make_pair( FMT("derive#" << trait_name), path ));
    }
};

STATIC_DECORATOR("proc_macro_derive", Decorator_ProcMacroDerive)



void Expand_ProcMacro(::AST::Crate& crate)
{
    // Create the following module:
    // ```
    // mod `proc_macro#` {
    //   extern crate proc_macro;
    //   fn main() {
    //     self::proc_macro::main(&::`proc_macro#`::MACROS);
    //   }
    //   static TESTS: [proc_macro::MacroDesc; _] = [
    //     proc_macro::MacroDesc { name: "deriving_Foo", handler: ::path::to::foo }
    //     ];
    // }
    // ```

    // ---- main function ----
    auto main_fn = ::AST::Function { Span(), {}, ABI_RUST, false, false, false, TypeRef(TypeRef::TagUnit(), Span()), {} };
    {
        auto call_node = NEWNODE(_CallPath,
                ::AST::Path("proc_macro", { ::AST::PathNode("main") }),
                ::make_vec1(
                    NEWNODE(_UniOp, ::AST::ExprNode_UniOp::REF,
                        NEWNODE(_NamedValue, ::AST::Path("", { ::AST::PathNode("proc_macro#"), ::AST::PathNode("MACROS") }))
                        )
                    )
                );
        main_fn.set_code( mv$(call_node) );
    }


    // ---- test list ----
    ::std::vector< ::AST::ExprNodeP>    test_nodes;

    for(const auto& desc : crate.m_proc_macros)
    {
        ::AST::ExprNode_StructLiteral::t_values   desc_vals;
        // `name: "foo",`
        desc_vals.push_back({ {}, "name", NEWNODE(_String,  desc.first) });
        // `handler`: ::foo
        desc_vals.push_back({ {}, "handler", NEWNODE(_NamedValue, AST::Path(desc.second)) });

        test_nodes.push_back( NEWNODE(_StructLiteral,  ::AST::Path("proc_macro", { ::AST::PathNode("MacroDesc")}), nullptr, mv$(desc_vals) ) );
    }
    auto* tests_array = new ::AST::ExprNode_Array(mv$(test_nodes));

    size_t test_count = tests_array->m_values.size();
    auto tests_list = ::AST::Static { ::AST::Static::Class::STATIC,
        TypeRef(TypeRef::TagSizedArray(), Span(),
                TypeRef(Span(), ::AST::Path("proc_macro", { ::AST::PathNode("MacroDesc") })),
                ::std::shared_ptr<::AST::ExprNode>( new ::AST::ExprNode_Integer(test_count, CORETYPE_UINT) )
               ),
        ::AST::Expr( mv$(tests_array) )
        };

    // ---- module ----
    auto newmod = ::AST::Module { ::AST::Path("", { ::AST::PathNode("proc_macro#") }) };
    // - TODO: These need to be loaded too.
    //  > They don't actually need to exist here, just be loaded (and use absolute paths)
    newmod.add_ext_crate(false, "proc_macro", "proc_macro", {});

    newmod.add_item(false, "main", mv$(main_fn), {});
    newmod.add_item(false, "MACROS", mv$(tests_list), {});

    crate.m_root_module.add_item(false, "proc_macro#", mv$(newmod), {});
    crate.m_lang_items["mrustc-main"] = ::AST::Path("", { AST::PathNode("proc_macro#"), AST::PathNode("main") });
}

enum class TokenClass
{
    Symbol = 0,
    Ident = 1,
    Lifetime = 2,
    String = 3,
    ByteString = 4, // String
    CharLit = 5,    // v128
    UnsignedInt = 6,
    SignedInt = 7,
    Float = 8,
    Fragment = 9,
};
enum class FragType
{
    Ident = 0,
    Tt = 1,

    Path = 2,
    Type = 3,

    Expr = 4,
    Statement = 5,
    Block = 6,
    Pattern = 7,
};
struct ProcMacroInv:
    public TokenStream
{
    Span    m_parent_span;

#ifdef WIN32
    HANDLE  child_handle;
    HANDLE  child_stdin;
    HANDLE  child_stdout;
#else
    // POSIX
    pid_t   child_pid;  // Questionably needed
     int    child_stdin;
     int    child_stdout;
    // NOTE: stderr stays as our stderr
#endif
    bool    m_eof_hit = false;

public:
    ProcMacroInv(const Span& sp, const char* executable, const char* macro_name);
    ProcMacroInv(const ProcMacroInv&) = delete;
    ProcMacroInv(ProcMacroInv&&);
    ProcMacroInv& operator=(const ProcMacroInv&) = delete;
    ProcMacroInv& operator=(ProcMacroInv&&);
    virtual ~ProcMacroInv();

    bool check_good();
    void send_done() {
        send_symbol("");
        DEBUG("Input tokens sent");
    }
    void send_symbol(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Symbol));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_ident(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Ident));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_lifetime(const char* val) {
        this->send_u8(static_cast<uint8_t>(TokenClass::Lifetime));
        this->send_bytes(val, ::std::strlen(val));
    }
    void send_string(const ::std::string& s) {
        this->send_u8(static_cast<uint8_t>(TokenClass::String));
        this->send_bytes(s.data(), s.size());
    }
    void send_bytestring(const ::std::string& s);
    void send_char(uint32_t ch);
    void send_int(eCoreType ct, int64_t v);
    void send_float(eCoreType ct, double v);
    //void send_fragment();

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
    virtual Ident::Hygiene realGetHygiene() const override;
private:
    Token realGetToken_();
    void send_u8(uint8_t v);
    void send_bytes(const void* val, size_t size);
    void send_v128u(uint64_t val);

    uint8_t recv_u8();
    ::std::string recv_bytes();
    uint64_t recv_v128u();
};

ProcMacroInv ProcMacro_Invoke_int(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path)
{
    // 1. Locate macro in HIR list
    const auto& crate_name = mac_path.front();
    const auto& ext_crate = crate.m_extern_crates.at(crate_name);
    // TODO: Ensure that this macro is in the listed crate.

    // 2. Get executable and macro name
    ::std::string   proc_macro_exe_name = (ext_crate.m_filename + "-plugin");
    ::std::string   mac_name = mac_path.at(1);
    for(size_t i = 2; i < mac_path.size(); i ++)
    {
        mac_name += "::";
        mac_name += mac_path[i];
    }

    // 3. Create ProcMacroInv
    return ProcMacroInv(sp, proc_macro_exe_name.c_str(), mac_name.c_str());
}


namespace {
    struct Visitor/*:
        public AST::NodeVisitor*/
    {
        const Span& sp;
        ProcMacroInv&   m_pmi;
        Visitor(const Span& sp, ProcMacroInv& pmi):
            sp(sp),
            m_pmi(pmi)
        {
        }

        void visit_type(const ::TypeRef& ty)
        {
            // TODO: Correct handling of visit_type
            TU_MATCHA( (ty.m_data), (te),
            (None,
                BUG(sp, ty);
                ),
            (Any,
                m_pmi.send_symbol("_");
                ),
            (Bang,
                m_pmi.send_symbol("!");
                ),
            (Unit,
                m_pmi.send_symbol("(");
                m_pmi.send_symbol(")");
                ),
            (Macro,
                TODO(sp, "proc_macro send macro type - " << ty);
                ),
            (Primitive,
                TODO(sp, "proc_macro send primitive - " << ty);
                ),
            (Function,
                TODO(sp, "proc_macro send function - " << ty);
                ),
            (Tuple,
                m_pmi.send_symbol("(");
                for(const auto& st : te.inner_types)
                {
                    this->visit_type(st);
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol(")");
                ),
            (Borrow,
                m_pmi.send_symbol("&");
                if( te.is_mut )
                    m_pmi.send_ident("mut");
                this->visit_type(*te.inner);
                ),
            (Pointer,
                m_pmi.send_symbol("*");
                if( te.is_mut )
                    m_pmi.send_ident("mut");
                else
                    m_pmi.send_ident("const");
                this->visit_type(*te.inner);
                ),
            (Array,
                m_pmi.send_symbol("[");
                this->visit_type(*te.inner);
                m_pmi.send_symbol(";");
                this->visit_node(*te.size);
                m_pmi.send_symbol("]");
                ),
            (Generic,
                // TODO: This may already be resolved?... Wait, how?
                m_pmi.send_ident(te.name.c_str());
                ),
            (Path,
                this->visit_path(te.path);
                ),
            (TraitObject,
                m_pmi.send_symbol("(");
                if( te.hrls.size() > 0 )
                {
                    m_pmi.send_ident("for");
                    m_pmi.send_symbol("<");
                    for(const auto& v : te.hrls)
                    {
                        m_pmi.send_lifetime(v.c_str());
                        m_pmi.send_symbol(",");
                    }
                    m_pmi.send_symbol(">");
                }
                for(const auto& t : te.traits)
                {
                    this->visit_path(t);
                    m_pmi.send_symbol("+");
                }
                m_pmi.send_symbol(")");
                ),
            (ErasedType,
                m_pmi.send_ident("impl");
                if( te.hrls.size() > 0 )
                {
                    m_pmi.send_ident("for");
                    m_pmi.send_symbol("<");
                    for(const auto& v : te.hrls)
                    {
                        m_pmi.send_lifetime(v.c_str());
                        m_pmi.send_symbol(",");
                    }
                    m_pmi.send_symbol(">");
                }
                for(const auto& t : te.traits)
                {
                    this->visit_path(t);
                    m_pmi.send_symbol("+");
                }
                )
            )
        }

        void visit_path(const AST::Path& path, bool is_expr=false)
        {
            const ::std::vector<AST::PathNode>*  nodes = nullptr;
            TU_MATCHA( (path.m_class), (pe),
            (Invalid,
                BUG(sp, "Invalid path");
                ),
            (Local,
                m_pmi.send_ident(pe.name.c_str());
                ),
            (Relative,
                // TODO: Send hygiene information
                nodes = &pe.nodes;
                ),
            (Self,
                m_pmi.send_ident("self");
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                ),
            (Super,
                for(unsigned i = 0; i < pe.count; i ++)
                {
                    m_pmi.send_ident("super");
                    m_pmi.send_symbol("::");
                }
                nodes = &pe.nodes;
                ),
            (Absolute,
                m_pmi.send_symbol("::");
                m_pmi.send_string(pe.crate.c_str());
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                ),
            (UFCS,
                m_pmi.send_symbol("<");
                this->visit_type(*pe.type);
                if( pe.trait )
                {
                    m_pmi.send_ident("as");
                    this->visit_path(*pe.trait);
                }
                m_pmi.send_symbol(">");
                m_pmi.send_symbol("::");
                nodes = &pe.nodes;
                )
            )
            bool first = true;
            for(const auto& e : *nodes)
            {
                if(!first)
                    m_pmi.send_symbol("::");
                first = false;
                m_pmi.send_ident(e.name().c_str());
                if( ! e.args().is_empty() )
                {
                    if( is_expr )
                        m_pmi.send_symbol("::");
                    m_pmi.send_symbol("<");
                    for(const auto& l : e.args().m_lifetimes)
                    {
                        m_pmi.send_lifetime(l.c_str());
                        m_pmi.send_symbol(",");
                    }
                    for(const auto& t : e.args().m_types)
                    {
                        this->visit_type(t);
                        m_pmi.send_symbol(",");
                    }
                    for(const auto& a : e.args().m_assoc)
                    {
                        m_pmi.send_ident(a.first.c_str());
                        m_pmi.send_symbol("=");
                        this->visit_type(a.second);
                        m_pmi.send_symbol(",");
                    }
                    m_pmi.send_symbol(">");
                }
            }
        }
        void visit_params(const AST::GenericParams& params)
        {
            if( params.ty_params().size() > 0 || params.lft_params().size() > 0 )
            {
                bool is_first = true;
                m_pmi.send_symbol("<");
                // Lifetimes
                for( const auto& p : params.lft_params() )
                {
                    if( !is_first )
                        m_pmi.send_symbol(",");
                    m_pmi.send_lifetime(p.c_str());
                    is_first = false;
                }
                // Types
                for( const auto& p : params.ty_params() )
                {
                    if( !is_first )
                        m_pmi.send_symbol(",");
                    m_pmi.send_ident(p.name().c_str());
                    if( !p.get_default().is_wildcard() )
                    {
                        m_pmi.send_symbol("=");
                        this->visit_type(p.get_default());
                    }
                    is_first = false;
                }
                m_pmi.send_symbol(">");
            }
        }
        void visit_bounds(const AST::GenericParams& params)
        {
            if( params.bounds().size() > 0 )
            {
                // TODO:
                TODO(Span(), "visit_bounds");
            }
        }
        void visit_node(const ::AST::ExprNode& e)
        {
            TODO(Span(), "visit_node");
        }
        void visit_nodes(const ::AST::Expr& e)
        {
            // TODO: Expressions!
            TODO(Span(), "visit_nodes");
        }
        void visit_struct(const ::std::string& name, bool is_pub, const ::AST::Struct& str)
        {
            if( is_pub ) {
                m_pmi.send_ident("pub");
            }

            m_pmi.send_ident("struct");
            m_pmi.send_ident(name.c_str());
            this->visit_params(str.params());
            TU_MATCH(AST::StructData, (str.m_data), (se),
            (Unit,
                this->visit_bounds(str.params());
                m_pmi.send_symbol(";");
                ),
            (Tuple,
                m_pmi.send_symbol("(");
                for( const auto& si : se.ents )
                {
                    if( si.m_is_public )
                        m_pmi.send_ident("pub");
                    this->visit_type(si.m_type);
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol(")");
                this->visit_bounds(str.params());
                m_pmi.send_symbol(";");
                ),
            (Struct,
                this->visit_bounds(str.params());
                m_pmi.send_symbol("{");

                for( const auto& si : se.ents )
                {
                    if( si.m_is_public )
                        m_pmi.send_ident("pub");
                    m_pmi.send_ident(si.m_name.c_str());
                    m_pmi.send_symbol(":");
                    this->visit_type(si.m_type);
                    m_pmi.send_symbol(",");
                }
                m_pmi.send_symbol("}");
                )
            )
        }
        void visit_enum(const ::std::string& name, bool is_pub, const ::AST::Enum& str)
        {
        }
        void visit_union(const ::std::string& name, bool is_pub, const ::AST::Union& str)
        {
        }
    };
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path, const ::std::string& item_name, const ::AST::Struct& i)
{
    // 1. Create ProcMacroInv instance
    auto pmi = ProcMacro_Invoke_int(sp, crate, mac_path);
    if( !pmi.check_good() )
        return ::std::unique_ptr<TokenStream>();
    // 2. Feed item as a token stream.
    Visitor(sp, pmi).visit_struct(item_name, false, i);
    pmi.send_done();
    // 3. Return boxed invocation instance
    return box$(pmi);
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path, const ::std::string& item_name, const ::AST::Enum& i)
{
    // 1. Create ProcMacroInv instance
    auto pmi = ProcMacro_Invoke_int(sp, crate, mac_path);
    if( !pmi.check_good() )
        return ::std::unique_ptr<TokenStream>();
    // 2. Feed item as a token stream.
    Visitor(sp, pmi).visit_enum(item_name, false, i);
    pmi.send_done();
    // 3. Return boxed invocation instance
    return box$(pmi);
}
::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path, const ::std::string& item_name, const ::AST::Union& i)
{
    // 1. Create ProcMacroInv instance
    auto pmi = ProcMacro_Invoke_int(sp, crate, mac_path);
    if( !pmi.check_good() )
        return ::std::unique_ptr<TokenStream>();
    // 2. Feed item as a token stream.
    Visitor(sp, pmi).visit_union(item_name, false, i);
    pmi.send_done();
    // 3. Return boxed invocation instance
    return box$(pmi);
}

ProcMacroInv::ProcMacroInv(const Span& sp, const char* executable, const char* macro_name):
    m_parent_span(sp)
{
#ifdef WIN32
#else
     int    stdin_pipes[2];
    pipe(stdin_pipes);
    this->child_stdin = stdin_pipes[1]; // Write end
     int    stdout_pipes[2];
    pipe(stdout_pipes);
    this->child_stdout = stdout_pipes[0]; // Read end

    posix_spawn_file_actions_t  file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_adddup2(&file_actions, stdin_pipes[0], 0);
    posix_spawn_file_actions_adddup2(&file_actions, stdout_pipes[1], 1);
    posix_spawn_file_actions_addclose(&file_actions, stdin_pipes[0]);
    posix_spawn_file_actions_addclose(&file_actions, stdin_pipes[1]);
    posix_spawn_file_actions_addclose(&file_actions, stdout_pipes[0]);
    posix_spawn_file_actions_addclose(&file_actions, stdout_pipes[1]);

    char*   argv[3] = { const_cast<char*>(executable), const_cast<char*>(macro_name), nullptr };
    char*   envp[] = { nullptr };
    int rv = posix_spawn(&this->child_pid, executable, &file_actions, nullptr, argv, envp);
    if( rv != 0 )
    {
        BUG(sp, "Error in posix_spawn - " << rv);
    }

    posix_spawn_file_actions_destroy(&file_actions);
    // Close the ends we don't care about.
    close(stdin_pipes[0]);
    close(stdout_pipes[1]);

#endif
}
ProcMacroInv::ProcMacroInv(ProcMacroInv&& x):
    m_parent_span(x.m_parent_span),
#ifdef WIN32
#else
    child_pid(x.child_pid),
    child_stdin(x.child_stdin),
    child_stdout(x.child_stdout)
#endif
{
#ifdef WIN32
#else
    x.child_pid = 0;
#endif
    DEBUG("");
}
ProcMacroInv& ProcMacroInv::operator=(ProcMacroInv&& x)
{
    m_parent_span = x.m_parent_span;
#ifdef WIN32
#else
    child_pid = x.child_pid;
    child_stdin = x.child_stdin;
    child_stdout = x.child_stdout;

    x.child_pid = 0;
#endif
    DEBUG("");
    return *this;
}
ProcMacroInv::~ProcMacroInv()
{
#ifdef WIN32
#else
    if( this->child_pid != 0 )
    {
        DEBUG("Waiting for child " << this->child_pid << " to terminate");
        int status;
        waitpid(this->child_pid, &status, 0);
        close(this->child_stdout);
        close(this->child_stdin);
    }
#endif
}
bool ProcMacroInv::check_good()
{
    char    v;
    int rv = read(this->child_stdout, &v, 1);
    if( rv == 0 )
    {
        DEBUG("Unexpected EOF from child");
        return false;
    }
    if( rv < 0 )
    {
        DEBUG("Error reading from child, rv=" << rv << " " << strerror(errno));
        return false;
    }
    DEBUG("Child started, value = " << (int)v);
    if( v != 0 )
        return false;
    return true;
}
void ProcMacroInv::send_u8(uint8_t v)
{
#ifdef WIN32
#else
    write(this->child_stdin, &v, 1);
#endif
}
void ProcMacroInv::send_bytes(const void* val, size_t size)
{
    this->send_v128u( static_cast<uint64_t>(size) );
#ifdef WIN32
#else
    write(this->child_stdin, val, size);
#endif
}
void ProcMacroInv::send_v128u(uint64_t val)
{
    while( val >= 128 ) {
        this->send_u8( static_cast<uint8_t>(val & 0x7F) | 0x80 );
        val >>= 7;
    }
    this->send_u8( static_cast<uint8_t>(val & 0x7F) );
}
uint8_t ProcMacroInv::recv_u8()
{
    uint8_t v;
    if( read(this->child_stdout, &v, 1) != 1 )
        BUG(this->m_parent_span, "Unexpected EOF while reading from child process");
    return v;
}
::std::string ProcMacroInv::recv_bytes()
{
    auto len = this->recv_v128u();
    ASSERT_BUG(this->m_parent_span, len < SIZE_MAX, "Oversized string from child process");
    ::std::string   val;
    val.resize(len);
    size_t  ofs = 0, rem = len;
    while( rem > 0 )
    {
        auto n = read(this->child_stdout, &val[ofs], rem);
        if( n == 0 ) {
            BUG(this->m_parent_span, "Unexpected EOF while reading from child process");
        }
        if( n < 0 ) {
            BUG(this->m_parent_span, "Error while reading from child process");
        }
        assert(static_cast<size_t>(n) <= rem);
        ofs += n;
        rem -= n;
    }

    return val;
}
uint64_t ProcMacroInv::recv_v128u()
{
    uint64_t    v = 0;
    unsigned    ofs = 0;
    for(;;)
    {
        auto b = recv_u8();
        v |= static_cast<uint64_t>(b) << ofs;
        if( (b & 0x80) == 0 )
            break;
        ofs += 7;
    }
    return v;
}

Position ProcMacroInv::getPosition() const {
    return Position();
}
Token ProcMacroInv::realGetToken() {
    auto rv = this->realGetToken_();
    DEBUG(rv);
    return rv;
}
Token ProcMacroInv::realGetToken_() {
    if( m_eof_hit )
        return Token(TOK_EOF);
    uint8_t v = this->recv_u8();

    switch( static_cast<TokenClass>(v) )
    {
    case TokenClass::Symbol: {
        auto val = this->recv_bytes();
        if( val == "" ) {
            m_eof_hit = true;
            return Token(TOK_EOF);
        }
        auto t = Lex_FindOperator(val);
        ASSERT_BUG(this->m_parent_span, t != TOK_NULL, "Unknown symbol from child process - " << val);
        return t;
        }
    case TokenClass::Ident: {
        auto val = this->recv_bytes();
        auto t = Lex_FindReservedWord(val);
        if( t != TOK_NULL )
            return t;
        return Token(TOK_IDENT, mv$(val));
        }
    case TokenClass::Lifetime: {
        auto val = this->recv_bytes();
        return Token(TOK_LIFETIME, mv$(val));
        }
    case TokenClass::String: {
        auto val = this->recv_bytes();
        return Token(TOK_STRING, mv$(val));
        }
    case TokenClass::ByteString: {
        auto val = this->recv_bytes();
        return Token(TOK_BYTESTRING, mv$(val));
        }
    case TokenClass::CharLit: {
        auto val = this->recv_v128u();
        return Token(static_cast<uint64_t>(val), CORETYPE_CHAR);
        }
    case TokenClass::UnsignedInt: {
        ::eCoreType ty;
        switch(this->recv_u8())
        {
        case   0: ty = CORETYPE_ANY;  break;
        case   1: ty = CORETYPE_UINT; break;
        case   8: ty = CORETYPE_U8;  break;
        case  16: ty = CORETYPE_U16; break;
        case  32: ty = CORETYPE_U32; break;
        case  64: ty = CORETYPE_U64; break;
        case 128: ty = CORETYPE_U128; break;
        default:    BUG(this->m_parent_span, "Invalid integer size from child process");
        }
        auto val = this->recv_v128u();
        return Token(static_cast<uint64_t>(val), ty);
        }
    case TokenClass::SignedInt:
    case TokenClass::Float:
    case TokenClass::Fragment:
        TODO(this->m_parent_span, "Handle ints/floats/fragments from child process");
    }
    BUG(this->m_parent_span, "Invalid token class from child process");

    throw "";
}
Ident::Hygiene ProcMacroInv::realGetHygiene() const {
    return Ident::Hygiene();
}

