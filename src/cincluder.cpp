#pragma warning(push)
#pragma warning(disable:4996)
#pragma warning(disable:4244)
#pragma warning(disable:4291)
#pragma warning(disable:4146)

#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/GraphWriter.h"

#pragma warning(pop)

#include <functional>

using namespace std;
using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// ***************************************************************************
//          プリプロセッサからのコールバック処理
// ***************************************************************************

class cincluder : public PPCallbacks {
private:
    Preprocessor &PP;

	typedef unsigned uid_t;
	typedef ::std::list< uid_t > uid_list;

	struct header
	{
		header() : angled(false) {}
		header(const ::std::string& n, bool angled_) : name(n), angled(angled_) {}
		::std::string name;
		bool angled;
		uid_list include;
	};
	typedef ::std::map< uid_t, header > Map;
	typedef ::std::vector< uid_t > Angled;
	typedef ::std::map< uid_t, uid_list > Depend;
	Map m_includes;
	Depend m_depends;
	Angled m_angleds;
	uid_t m_root;
	::std::string m_output;
	bool m_reportRedundant;
	bool m_ignoreSystem;
public:
	cincluder(Preprocessor &pp) : PP(pp) {}
	cincluder(Preprocessor &pp, const ::std::string& output, bool Redundant, bool ignoreSystem)
		: PP(pp), m_output(output), m_reportRedundant(Redundant), m_ignoreSystem(ignoreSystem) {}

	const header& getHeader(uid_t h)
	{
		return m_includes[h];
	}
	
	::std::string getFilePath(uid_t h)
	{
		return m_includes[h].name;
	}
	::std::string getFileName(uid_t h)
	{
		::std::string path = getFilePath(h);
		size_t dpos1 = path.rfind('\\');
		size_t dpos2 = path.rfind('/');
		if( dpos1 == ::std::string::npos ) dpos1 = 0;
		if( dpos2 == ::std::string::npos ) dpos2 = 0;
		const size_t dpos = (::std::max)(dpos1, dpos2) + 1;

		return path.substr( dpos );
	}
	::std::string getFileName(const header& h)
	{
		::std::string path = h.name;
		size_t dpos1 = path.rfind('\\');
		size_t dpos2 = path.rfind('/');
		if( dpos1 == ::std::string::npos ) dpos1 = 0;
		if( dpos2 == ::std::string::npos ) dpos2 = 0;
		const size_t dpos = (::std::max)(dpos1, dpos2) + 1;

		return path.substr(dpos);
	}

	void printRoot(uid_t h, int indent, bool expand)
	{
		if( indent > 2 ) return;
		for( int i = 0; i < indent; ++i ) errs() << "  ";
		if( expand ) errs() << "+ ";
		else errs() << "  ";
		errs() << getFileName(h) << "\n";

		if( m_depends.find(h) != m_depends.end() )
		{
			auto depend = m_depends[h];
			if( depend.size() > 1 ) ++indent;
			for( auto d : depend )
			{
				printRoot(d, indent, (depend.size() > 1));
			}
		}
	}

	void report()
	{
		for( auto h : m_depends )
		{
			if( h.second.size() > 1 )
			{
				errs() << getFileName(h.first) << " is already include by \n";
				for( auto d : h.second )
				{
					printRoot(d, 1, true);
				}
			}
		}
	}

	void writeID(raw_ostream& OS, uid_t h)
	{
		OS << "header_";
		OS << h;
	}

	void dot()
	{
		if( m_output.empty() ) return;
		std::error_code EC;
		llvm::raw_fd_ostream OS(m_output, EC, llvm::sys::fs::F_Text);
		if( EC ) 
		{
			PP.getDiagnostics().Report(diag::err_fe_error_opening) << m_output
				<< EC.message();
			return;
		}
		const char* endl = "\n";

		const ::std::string color = ", color = \"#FF0000\"";

		OS << "digraph \"dependencies\" {" << endl;

		for( auto inc : m_includes )
		{
			writeID(OS, inc.first);
			OS << " [ shape=\"box\", label=\"";
			OS << DOT::EscapeString(getFileName(inc.second));
			OS << "\"";
			if( m_depends[inc.first].size() > 1 )
			{
				OS << color;
			}
			OS << "];" << endl;
		}

		for( auto h : m_depends )
		{
			for(auto depend : h.second)
			{
				writeID(OS, h.first);
				OS << " -> ";
				writeID(OS, depend);
				OS << endl;
			}
		}

		OS << "}" << endl;
	}

	void EndOfMainFile() override
	{
		if( m_reportRedundant )
		{
			report();
		}
		dot();
	}

    void InclusionDirective(SourceLocation HashLoc,
                          const Token &IncludeTok,
                          llvm::StringRef FileName, 
                          bool IsAngled,
                          CharSourceRange FilenameRange,
                          const FileEntry *File,
                          llvm::StringRef SearchPath,
                          llvm::StringRef RelativePath,
                          const Module *Imported) override
	{
		if( File == nullptr ) return;
		if( m_ignoreSystem && IsAngled ) return;

		SourceManager& SM = PP.getSourceManager();
		const FileEntry* pFromFile = SM.getFileEntryForID(SM.getFileID(SM.getExpansionLoc(HashLoc)));
		if( pFromFile == nullptr ) return;

#if 0
		const auto h = hash()(File->getName());
		const auto p = hash()(pFromFile->getName());
		//errs() << File->getName() << "( " << File->getUID() << " )\n";
#else
		const auto h = File->getUID();
		const auto p = pFromFile->getUID();
#endif

		{
			auto it = m_includes.find(p);
			if(it == m_includes.end())
			{
				if(::std::find(m_angleds.begin(), m_angleds.end(), p) == m_angleds.end())
				{
					m_root = p;
					m_includes.insert(::std::make_pair(p, header(pFromFile->getName(), false)));
				}
				else
				{
					m_angleds.push_back(h);
					return;
				}
			}
			if(it != m_includes.end())
			{
				if(it->second.angled)
				{
					m_angleds.push_back(h);
					return;
				}
				else
				{
					it->second.include.push_back(h);
				}
			}
		}
		{
			if(m_includes.find(h) == m_includes.end())
			{
				m_includes.insert(::std::make_pair(h, header(File->getName(), IsAngled)));
			}

			auto it = m_depends.find(h);
			if( it != m_depends.end() )
			{
				it->second.push_back(p);
			}
			else
			{
				uid_list a;
				a.push_back(p);
				m_depends.insert(::std::make_pair(h, a));
			}
		}


#if 0
        errs() << "InclusionDirective : ";
        if (File) {
            if (IsAngled)   errs() << "<" << File->getName() << ">\n";
            else            errs() << "\"" << File->getName() << "\"\n";
        } else {
            errs() << "not found file ";
            if (IsAngled)   errs() << "<" << FileName << ">\n";
            else            errs() << "\"" << FileName << "\"\n";
        }
#endif
    }
};

namespace
{

static cl::OptionCategory CincluderCategory("cincluder");
static cl::opt<::std::string> DotFile("dot", cl::init("cincluder.dot"), cl::desc("output dot file"), cl::cat(CincluderCategory));
static cl::opt<bool> Redundant("report-redundant", cl::desc("report redundant include file"), cl::cat(CincluderCategory));
static cl::opt<bool> IgnoreSystem("ignore-system", cl::desc("ignore system include file"), cl::cat(CincluderCategory));

}

class ExampleASTConsumer : public ASTConsumer {
private:
public:
	explicit ExampleASTConsumer(CompilerInstance *CI) {
		// プリプロセッサからのコールバック登録
		Preprocessor &PP = CI->getPreprocessor();
		PP.addPPCallbacks(llvm::make_unique<cincluder>(PP, DotFile, Redundant, IgnoreSystem));
		//AttachDependencyGraphGen(PP, "test.dot", "");
	}
};

class ExampleFrontendAction : public SyntaxOnlyAction /*ASTFrontendAction*/ {
public:
	virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) {
		return llvm::make_unique<ExampleASTConsumer>(&CI); // pass CI pointer to ASTConsumer
	}
};

int main(int argc, const char** argv)
{
	CommonOptionsParser op(argc, argv, CincluderCategory);
	ClangTool Tool(op.getCompilations(), op.getSourcePathList());
	return Tool.run(newFrontendActionFactory<ExampleFrontendAction>().get());
}
