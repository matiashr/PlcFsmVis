// stvis: Visualize IEC 61131-3 state machines in .st files as Graphviz DOT
//
// PEG grammar (simplified):
//   File         <- (CaseStmt / AnyChar)*
//   CaseStmt     <- CASE Expr OF Branches END_CASE ';'?
//   Expr         <- (captured until OF; examined for "state")
//   Branches     <- (LabelBranch)* ElseBranch?
//   LabelBranch  <- QualName ':' StmtList
//   ElseBranch   <- ELSE StmtList
//   StmtList     <- (IfStmt / StateAssign / CompoundSkip / SimpleSkip)*
//   IfStmt       <- IF Cond THEN StmtList (ELSIF Cond THEN StmtList)* (ELSE StmtList)? END_IF ';'?
//   Cond         <- (captured until THEN)
//   StateAssign  <- Ident[== state var from CASE expr] ':=' QualName ';'
//   QualName     <- Ident ('.' Ident)?
//   CompoundSkip <- (IF/FOR/WHILE/CASE/REPEAT) ... matching END_xxx
//   SimpleSkip   <- (not branch-label / not ELSE / not END_CASE) until ';'
//
// A CASE is only emitted as a graph when at least one branch actually assigns
// to the same variable(s) used in the CASE expression.

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

struct Transition {
    std::string from, to, cond;
    bool        guarded = false;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class STParser {
public:
    std::vector<std::string>   state_order;
    std::set<std::string>      states_with_guard;
    std::vector<Transition>    transitions;
    std::vector<size_t>        case_lines;   // source line of each committed CASE

    explicit STParser(const std::string& src) : src_(src), pos_(0) {}

    void parse() { parse_file(); }

    void emit_dot(std::ostream& out, const std::string& filename = "") const {
        if (state_order.empty() || transitions.empty()) return;

        out << "digraph finite_state_machine {\n";
        out << "\tfontname=\"Helvetica,Arial,sans-serif\"\n";
        out << "\tnode [fontname=\"Helvetica,Arial,sans-serif\"]\n";
        out << "\tedge [fontname=\"Helvetica,Arial,sans-serif\"]\n";
        out << "\trankdir=LR;\n";

        if (!filename.empty() && !case_lines.empty()) {
            out << "\tlabel=\"";
            for (size_t i = 0; i < case_lines.size(); i++) {
                if (i > 0) out << "\\n";
                out << filename << ":" << case_lines[i];
            }
            out << "\";\n";
            out << "\tlabelloc=t;\n";
        }

        out << "\tnode [shape = doublecircle]; " << state_order[0] << " ;\n";

        if (state_order.size() > 1) {
            out << "\tnode [shape = circle]";
            for (size_t i = 1; i < state_order.size(); i++)
                out << " " << state_order[i];
            out << ";\n";
        }

        for (const auto& state : state_order) {
            if (states_with_guard.count(state))
                out << "\t" << state << "->" << state << ";\n";

            for (const auto& t : transitions) {
                if (t.from != state) continue;
                out << "\t" << t.from << "->" << t.to;
                if (!t.cond.empty()) {
                    bool needs_quote = false;
                    for (char c : t.cond)
                        if (!isalnum((unsigned char)c) && c != '_') { needs_quote = true; break; }
                    out << "[label=";
                    if (needs_quote) out << "\"";
                    out << t.cond;
                    if (needs_quote) out << "\"";
                    out << "]";
                }
                out << ";\n";
            }
        }

        out << " \n}\n";
    }

private:
    std::string src_;
    size_t      pos_;

    // Per-CASE temporaries; merged into the public members only when transitions found
    std::vector<std::string> cur_state_vars_;   // identifiers from CASE expr containing "state"
    std::vector<std::string> cur_states_;
    std::set<std::string>    cur_guards_;
    std::vector<Transition>  cur_trans_;
    size_t                   cur_line_ = 0;

    size_t line_of(size_t pos) const {
        size_t line = 1;
        for (size_t i = 0; i < pos && i < src_.size(); i++)
            if (src_[i] == '\n') line++;
        return line;
    }

    // -----------------------------------------------------------------------
    // Primitives
    // -----------------------------------------------------------------------

    char peek(size_t off = 0) const {
        return (pos_ + off < src_.size()) ? src_[pos_ + off] : '\0';
    }
    bool at_end() const { return pos_ >= src_.size(); }

    static bool is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

    void skip_ws() {
        while (!at_end() && isspace((unsigned char)peek())) pos_++;
    }

    void skip_one_comment() {
        if (peek() == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n') pos_++;
            return;
        }
        if (peek() == '(' && peek(1) == '*') {
            pos_ += 2;
            while (!at_end() && !(peek() == '*' && peek(1) == ')')) pos_++;
            if (!at_end()) pos_ += 2;
            return;
        }
    }

    void skip_ws_cmts() {
        while (!at_end()) {
            if (isspace((unsigned char)peek())) { skip_ws(); continue; }
            if ((peek() == '/' && peek(1) == '/') || (peek() == '(' && peek(1) == '*'))
                { skip_one_comment(); continue; }
            break;
        }
    }

    bool try_kw(const std::string& kw) {
        size_t saved = pos_;
        skip_ws_cmts();
        if (pos_ + kw.size() > src_.size()) { pos_ = saved; return false; }
        for (size_t i = 0; i < kw.size(); i++)
            if (tolower((unsigned char)src_[pos_+i]) != tolower((unsigned char)kw[i]))
                { pos_ = saved; return false; }
        size_t nx = pos_ + kw.size();
        if (nx < src_.size() && is_word(src_[nx])) { pos_ = saved; return false; }
        pos_ = nx;
        return true;
    }

    bool peek_kw(const std::string& kw) const {
        size_t p = pos_;
        while (p < src_.size() && isspace((unsigned char)src_[p])) p++;
        while (p < src_.size()) {
            if (p+1<src_.size() && src_[p]=='/' && src_[p+1]=='/') {
                while (p<src_.size() && src_[p]!='\n') p++;
                while (p<src_.size() && isspace((unsigned char)src_[p])) p++;
                continue;
            }
            if (p+1<src_.size() && src_[p]=='(' && src_[p+1]=='*') {
                p+=2; while(p+1<src_.size() && !(src_[p]=='*' && src_[p+1]==')')) p++; p+=2;
                while(p<src_.size() && isspace((unsigned char)src_[p])) p++;
                continue;
            }
            break;
        }
        if (p + kw.size() > src_.size()) return false;
        for (size_t i = 0; i < kw.size(); i++)
            if (tolower((unsigned char)src_[p+i]) != tolower((unsigned char)kw[i])) return false;
        size_t nx = p + kw.size();
        return !(nx < src_.size() && is_word(src_[nx]));
    }

    bool try_char(char c) {
        skip_ws_cmts();
        if (!at_end() && peek() == c) { pos_++; return true; }
        return false;
    }

    bool try_ident(std::string& out) {
        size_t saved = pos_;
        skip_ws_cmts();
        if (at_end() || (!isalpha((unsigned char)peek()) && peek() != '_'))
            { pos_ = saved; return false; }
        size_t start = pos_;
        while (!at_end() && is_word(peek())) pos_++;
        out = src_.substr(start, pos_ - start);
        return true;
    }

    bool try_qual_name(std::string& out) {
        std::string first;
        if (!try_ident(first)) return false;
        size_t after_first = pos_;
        skip_ws_cmts();
        if (!at_end() && peek() == '.') {
            pos_++;
            std::string second;
            if (try_ident(second)) { out = first + "." + second; return true; }
        }
        pos_ = after_first;
        out = first;
        return true;
    }

    // -----------------------------------------------------------------------
    // Utilities
    // -----------------------------------------------------------------------

    static std::string short_name(const std::string& q) {
        size_t d = q.rfind('.');
        return (d != std::string::npos) ? q.substr(d+1) : q;
    }

    static std::string trim(const std::string& s) {
        size_t l = 0, r = s.size();
        while (l < r && isspace((unsigned char)s[l])) l++;
        while (r > l && isspace((unsigned char)s[r-1])) r--;
        return s.substr(l, r-l);
    }

    static bool icontains(const std::string& s, const std::string& sub) {
        if (sub.size() > s.size()) return false;
        auto it = std::search(s.begin(), s.end(), sub.begin(), sub.end(),
            [](char a, char b){ return tolower((unsigned char)a)==tolower((unsigned char)b); });
        return it != s.end();
    }

    static bool iequals(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    }

    // Extract identifier tokens from expr that contain "state" (case-insensitive).
    // These are the variables we expect to see on the LHS of state assignments.
    static std::vector<std::string> extract_state_vars(const std::string& expr) {
        std::vector<std::string> vars;
        size_t i = 0;
        while (i < expr.size()) {
            if (isalpha((unsigned char)expr[i]) || expr[i] == '_') {
                size_t start = i;
                while (i < expr.size() && (isalnum((unsigned char)expr[i]) || expr[i] == '_')) i++;
                std::string tok = expr.substr(start, i - start);
                if (icontains(tok, "state")) vars.push_back(tok);
            } else { i++; }
        }
        return vars;
    }

    void cur_add_state(const std::string& name) {
        for (const auto& s : cur_states_) if (s == name) return;
        cur_states_.push_back(name);
    }

    void cur_record_transition(const std::string& from, const std::string& to,
                               const std::string& cond, bool guarded) {
        cur_trans_.push_back({from, to, cond, guarded});
        if (guarded) cur_guards_.insert(from);
    }

    // Commit current CASE results into the public collections.
    void commit_case() {
        for (const auto& s : cur_states_) {
            if (std::find(state_order.begin(), state_order.end(), s) == state_order.end())
                state_order.push_back(s);
        }
        states_with_guard.insert(cur_guards_.begin(), cur_guards_.end());
        transitions.insert(transitions.end(), cur_trans_.begin(), cur_trans_.end());
        case_lines.push_back(cur_line_);
    }

    // Lookahead: next token is a CASE branch label (QualName ':' not followed by '=')
    bool peek_branch_label() const {
        size_t p = pos_;
        while (p < src_.size() && isspace((unsigned char)src_[p])) p++;
        while (p < src_.size()) {
            if (p+1<src_.size() && src_[p]=='/' && src_[p+1]=='/') {
                while (p<src_.size() && src_[p]!='\n') p++;
                while (p<src_.size() && isspace((unsigned char)src_[p])) p++;
                continue;
            }
            if (p+1<src_.size() && src_[p]=='(' && src_[p+1]=='*') {
                p+=2; while(p+1<src_.size() && !(src_[p]=='*' && src_[p+1]==')')) p++; p+=2;
                while(p<src_.size() && isspace((unsigned char)src_[p])) p++;
                continue;
            }
            break;
        }
        if (p >= src_.size() || (!isalpha((unsigned char)src_[p]) && src_[p] != '_')) return false;
        while (p < src_.size() && is_word(src_[p])) p++;
        while (p < src_.size() && isspace((unsigned char)src_[p])) p++;
        if (p < src_.size() && src_[p] == '.') {
            p++;
            while (p < src_.size() && isspace((unsigned char)src_[p])) p++;
            if (p >= src_.size() || (!isalpha((unsigned char)src_[p]) && src_[p] != '_')) return false;
            while (p < src_.size() && is_word(src_[p])) p++;
        }
        while (p < src_.size() && isspace((unsigned char)src_[p])) p++;
        if (p >= src_.size() || src_[p] != ':') return false;
        p++;
        return !(p < src_.size() && src_[p] == '=');
    }

    std::string capture_until_kw(const std::string& kw) {
        size_t start = pos_;
        while (!at_end() && !peek_kw(kw)) {
            if (peek() == '\'') {
                pos_++;
                while (!at_end() && peek() != '\'') pos_++;
                if (!at_end()) pos_++;
            } else { pos_++; }
        }
        return trim(src_.substr(start, pos_ - start));
    }

    void skip_to_matching_end(const std::string& open_kw, const std::string& close_kw) {
        int depth = 1;
        while (!at_end() && depth > 0) {
            skip_ws_cmts();
            if (peek_kw(open_kw))  { try_kw(open_kw);  depth++; continue; }
            if (peek_kw(close_kw)) { try_kw(close_kw); if (--depth == 0) break; continue; }
            pos_++;
        }
    }

    // -----------------------------------------------------------------------
    // Grammar rules
    // -----------------------------------------------------------------------

    void parse_file() {
        while (!at_end()) {
            if (!try_case_stmt()) pos_++;
        }
    }

    bool try_case_stmt() {
        size_t saved = pos_;
        if (!try_kw("CASE")) return false;
        // pos_ is now just past "CASE"; the keyword starts 4 chars before
        cur_line_ = line_of(pos_ - 4);

        std::string expr = capture_until_kw("OF");
        if (!try_kw("OF")) { pos_ = saved; return false; }

        if (!icontains(expr, "state")) {
            skip_to_matching_end("CASE", "END_CASE");
            try_char(';');
            return true;
        }

        // Set up per-CASE temporaries
        cur_state_vars_ = extract_state_vars(expr);
        cur_states_.clear();
        cur_guards_.clear();
        cur_trans_.clear();

        parse_branches();

        // Only keep this CASE if at least one branch actually assigned to the state var
        if (!cur_trans_.empty()) commit_case();

        try_kw("END_CASE");
        try_char(';');
        return true;
    }

    void parse_branches() {
        while (!at_end()) {
            skip_ws_cmts();
            if (peek_kw("END_CASE")) return;

            if (peek_kw("ELSE")) {
                try_kw("ELSE");
                while (!at_end() && !peek_kw("END_CASE"))
                    skip_stmt_any(false);
                return;
            }

            std::string label;
            if (!try_branch_label(label)) { pos_++; continue; }

            std::string state = short_name(label);
            cur_add_state(state);
            parse_branch_body(state);
        }
    }

    bool try_branch_label(std::string& out) {
        size_t saved = pos_;
        std::string name;
        if (!try_qual_name(name)) return false;
        skip_ws_cmts();
        if (at_end() || peek() != ':') { pos_ = saved; return false; }
        pos_++;
        if (!at_end() && src_[pos_] == '=') { pos_ = saved; return false; }
        out = name;
        return true;
    }

    void parse_branch_body(const std::string& state) {
        while (!at_end()) {
            skip_ws_cmts();
            if (peek_kw("END_CASE") || peek_kw("ELSE")) return;
            if (peek_branch_label()) return;

            if (try_if_stmt(state)) continue;
            if (try_state_assign(state, false, "")) continue;
            skip_stmt_any(true);
        }
    }

    bool try_if_stmt(const std::string& state) {
        size_t saved = pos_;
        if (!try_kw("IF")) return false;

        std::string cond = capture_until_kw("THEN");
        if (!try_kw("THEN")) { pos_ = saved; return false; }

        parse_if_body(state, true, cond);

        while (try_kw("ELSIF")) {
            std::string elsif_cond = capture_until_kw("THEN");
            try_kw("THEN");
            parse_if_body(state, true, elsif_cond);
        }
        if (try_kw("ELSE"))
            parse_if_body(state, true, "");

        try_kw("END_IF");
        try_char(';');
        return true;
    }

    void parse_if_body(const std::string& state, bool guarded, const std::string& cond) {
        while (!at_end()) {
            skip_ws_cmts();
            if (peek_kw("END_IF") || peek_kw("ELSIF") || peek_kw("ELSE")) return;
            if (peek_kw("END_CASE") || peek_branch_label()) return;

            if (try_if_stmt(state)) continue;
            if (try_state_assign(state, guarded, cond)) continue;
            skip_stmt_any(true);
        }
    }

    // Match: <ident that exactly matches one of cur_state_vars_> ':=' QualName ';'
    bool try_state_assign(const std::string& current_state, bool guarded, const std::string& cond) {
        size_t saved = pos_;
        std::string lhs;
        if (!try_ident(lhs)) return false;

        bool matches = std::any_of(cur_state_vars_.begin(), cur_state_vars_.end(),
                                   [&](const std::string& sv){ return iequals(sv, lhs); });
        if (!matches) { pos_ = saved; return false; }

        skip_ws_cmts();
        if (at_end() || peek() != ':') { pos_ = saved; return false; }
        pos_++;
        if (at_end() || peek() != '=') { pos_ = saved; return false; }
        pos_++;

        std::string rhs;
        if (!try_qual_name(rhs)) { pos_ = saved; return false; }

        try_char(';');

        std::string target = short_name(rhs);
        cur_record_transition(current_state, target, cond, guarded);
        return true;
    }

    void skip_stmt_any(bool inside_branch) {
        skip_ws_cmts();
        if (at_end()) return;
        if (peek_kw("END_CASE") || peek_kw("END_IF") || peek_kw("ELSIF") || peek_kw("ELSE")) return;
        if (inside_branch && peek_branch_label()) return;

        struct { const char* open; const char* close; } compound[] = {
            {"IF",     "END_IF"},
            {"CASE",   "END_CASE"},
            {"FOR",    "END_FOR"},
            {"WHILE",  "END_WHILE"},
            {"REPEAT", "END_REPEAT"},
        };
        for (auto& kw : compound) {
            if (try_kw(kw.open)) {
                skip_to_matching_end(kw.open, kw.close);
                try_char(';');
                return;
            }
        }

        while (!at_end()) {
            if (peek_kw("END_CASE") || peek_kw("END_IF") || peek_kw("ELSIF") || peek_kw("ELSE")) return;
            if (inside_branch && peek_branch_label()) return;
            if (peek() == ';') { pos_++; return; }
            pos_++;
        }
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: stvis <file.st> [output.dot]\n";
        return 1;
    }

    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "Cannot open: " << argv[1] << "\n"; return 1; }
    std::string src((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    STParser parser(src);
    parser.parse();

    if (parser.transitions.empty()) {
        std::cerr << "No state machines found (CASE <expr with 'state'> OF with assignments to that variable)\n";
        return 0;
    }

    if (argc >= 3) {
        std::ofstream out(argv[2]);
        if (!out) { std::cerr << "Cannot write: " << argv[2] << "\n"; return 1; }
        parser.emit_dot(out, argv[1]);
    } else {
        parser.emit_dot(std::cout, argv[1]);
    }
    return 0;
}
