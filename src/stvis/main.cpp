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
#include <unordered_map>
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
    struct ParsedCase {
        std::vector<std::string> states;
        std::set<std::string> guards;
        std::vector<Transition> transitions;
        size_t line = 0;
    };

    std::vector<ParsedCase> parsed_cases;

    explicit STParser(const std::string& src) : src_(src), pos_(0) {
        index_method_state_assignments();
    }

    void parse() { parse_file(); }

    void emit_dot(std::ostream& out, const std::string& filename = "") const {
        for (size_t idx = 0; idx < parsed_cases.size(); idx++) {
            const auto& pc = parsed_cases[idx];
            if (pc.states.empty() || pc.transitions.empty()) continue;

            out << "digraph finite_state_machine_" << (idx + 1) << " {\n";
            out << "\tfontname=\"Helvetica,Arial,sans-serif\"\n";
            out << "\tnode [fontname=\"Helvetica,Arial,sans-serif\"]\n";
            out << "\tedge [fontname=\"Helvetica,Arial,sans-serif\"]\n";
            out << "\trankdir=LR;\n";

            if (!filename.empty() && pc.line != 0) {
                out << "\tlabel=\"" << filename << ":" << pc.line << "\";\n";
                out << "\tlabelloc=t;\n";
            }

            out << "\tnode [shape = doublecircle]; " << pc.states[0] << " ;\n";

            if (pc.states.size() > 1) {
                out << "\tnode [shape = circle]";
                for (size_t i = 1; i < pc.states.size(); i++) out << " " << pc.states[i];
                out << ";\n";
            }

            for (const auto& state : pc.states) {
                if (pc.guards.count(state)) out << "\t" << state << "->" << state << ";\n";

                for (const auto& t : pc.transitions) {
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
            if (idx + 1 < parsed_cases.size()) out << "\n";
        }
    }

private:
    struct StateAssignTarget {
        std::string lhs;
        std::string rhs;
    };

    std::string src_;
    size_t      pos_;
    std::unordered_map<std::string, std::vector<StateAssignTarget>> method_state_assigns_;

    // Per-CASE temporaries; merged into the public members only when transitions found
    std::vector<std::string> cur_state_vars_;   // identifiers from CASE expr containing "state"
    std::vector<std::string> cur_states_;
    std::set<std::string>    cur_guards_;
    std::vector<Transition>  cur_trans_;
    size_t                   cur_line_ = 0;

    struct CaseContext {
        std::vector<std::string> state_vars;
        std::vector<std::string> states;
        std::set<std::string> guards;
        std::vector<Transition> transitions;
        size_t line = 0;
    };

    CaseContext save_case_context() const {
        return {cur_state_vars_, cur_states_, cur_guards_, cur_trans_, cur_line_};
    }

    void restore_case_context(CaseContext&& ctx) {
        cur_state_vars_ = std::move(ctx.state_vars);
        cur_states_ = std::move(ctx.states);
        cur_guards_ = std::move(ctx.guards);
        cur_trans_ = std::move(ctx.transitions);
        cur_line_ = ctx.line;
    }

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

    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    static bool ci_starts_with_at(const std::string& s, size_t pos, const std::string& kw) {
        if (pos + kw.size() > s.size()) return false;
        for (size_t i = 0; i < kw.size(); i++) {
            if (std::tolower((unsigned char)s[pos + i]) != std::tolower((unsigned char)kw[i])) return false;
        }
        return true;
    }

    void index_method_state_assignments() {
        size_t p = 0;
        while (p < src_.size()) {
            if (!ci_starts_with_at(src_, p, "METHOD")) { p++; continue; }
            if (p > 0 && is_word(src_[p - 1])) { p++; continue; }

            size_t q = p + 6;
            while (q < src_.size() && isspace((unsigned char)src_[q])) q++;
            if (q >= src_.size() || (!isalpha((unsigned char)src_[q]) && src_[q] != '_')) { p++; continue; }

            size_t name_start = q;
            while (q < src_.size() && is_word(src_[q])) q++;
            std::string method_name = to_lower(src_.substr(name_start, q - name_start));

            size_t body_start = q;
            size_t end_pos = std::string::npos;
            size_t search_from = body_start;
            while (search_from < src_.size()) {
                size_t e = src_.find("END_METHOD", search_from);
                if (e == std::string::npos) break;
                bool ok_before = (e == 0 || !is_word(src_[e - 1]));
                bool ok_after = (e + 10 >= src_.size() || !is_word(src_[e + 10]));
                if (ok_before && ok_after) { end_pos = e; break; }
                search_from = e + 1;
            }
            if (end_pos == std::string::npos) break;

            std::string body = src_.substr(body_start, end_pos - body_start);
            size_t bp = 0;
            while (bp < body.size()) {
                while (bp < body.size() && !(isalpha((unsigned char)body[bp]) || body[bp] == '_')) bp++;
                if (bp >= body.size()) break;

                std::string lhs;
                if (!parse_lhs_var(body, bp, lhs)) continue;

                while (bp < body.size() && isspace((unsigned char)body[bp])) bp++;
                if (bp + 1 >= body.size() || body[bp] != ':' || body[bp + 1] != '=') continue;
                bp += 2;

                while (bp < body.size() && isspace((unsigned char)body[bp])) bp++;
                if (bp >= body.size() || (!isalpha((unsigned char)body[bp]) && body[bp] != '_')) continue;

                size_t rhs_start = bp;
                while (bp < body.size() && is_word(body[bp])) bp++;
                if (bp < body.size() && body[bp] == '.') {
                    bp++;
                    while (bp < body.size() && isspace((unsigned char)body[bp])) bp++;
                    while (bp < body.size() && is_word(body[bp])) bp++;
                }
                std::string rhs = trim(body.substr(rhs_start, bp - rhs_start));
                if (rhs.empty()) continue;

                method_state_assigns_[method_name].push_back({lhs, short_name(rhs)});
            }

            p = end_pos + 10;
        }
    }

    // Extract identifier tokens from CASE expr.
    // These are the variables we expect to see on the LHS of assignments.
    static std::vector<std::string> extract_case_vars(const std::string& expr) {
        size_t p = 0;
        std::string lhs;
        if (parse_lhs_var(expr, p, lhs)) return {lhs};

        std::vector<std::string> vars;
        size_t i = p;
        while (i < expr.size()) {
            if (isalpha((unsigned char)expr[i]) || expr[i] == '_') {
                size_t start = i;
                while (i < expr.size() && (isalnum((unsigned char)expr[i]) || expr[i] == '_')) i++;
                std::string tok = expr.substr(start, i - start);
                if (std::find(vars.begin(), vars.end(), tok) == vars.end()) vars.push_back(tok);
            } else { i++; }
        }
        return vars;
    }

    static bool parse_lhs_var(const std::string& s, size_t& p, std::string& out) {
        while (p < s.size() && isspace((unsigned char)s[p])) p++;
        if (p >= s.size() || (!isalpha((unsigned char)s[p]) && s[p] != '_')) return false;

        auto parse_ident = [&](std::string& id) {
            if (p >= s.size() || (!isalpha((unsigned char)s[p]) && s[p] != '_')) return false;
            size_t start = p;
            while (p < s.size() && is_word(s[p])) p++;
            id = s.substr(start, p - start);
            return true;
        };

        std::string id;
        if (!parse_ident(id)) return false;
        out = id;

        while (true) {
            while (p < s.size() && isspace((unsigned char)s[p])) p++;
            if (p < s.size() && s[p] == '^') {
                p++;
                while (p < s.size() && isspace((unsigned char)s[p])) p++;
            }
            if (p < s.size() && s[p] == '.') {
                p++;
                while (p < s.size() && isspace((unsigned char)s[p])) p++;
                std::string next;
                if (!parse_ident(next)) return false;
                out = next;
                continue;
            }
            break;
        }
        return true;
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
        ParsedCase pc;
        pc.states = cur_states_;
        pc.guards = cur_guards_;
        pc.transitions = cur_trans_;
        pc.line = cur_line_;
        parsed_cases.push_back(std::move(pc));
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
            size_t before = pos_;
            if (!try_case_stmt() && pos_ == before) pos_++;
        }
    }

    bool try_case_stmt() {
        size_t saved = pos_;
        if (!try_kw("CASE")) return false;

        CaseContext parent_ctx = save_case_context();

        // pos_ is now just past "CASE"; the keyword starts 4 chars before
        cur_line_ = line_of(pos_ - 4);

        std::string expr = capture_until_kw("OF");
        if (!try_kw("OF")) {
            pos_ = saved;
            restore_case_context(std::move(parent_ctx));
            return false;
        }

        // Set up per-CASE temporaries
        cur_state_vars_ = extract_case_vars(expr);
        if (cur_state_vars_.empty()) {
            skip_to_matching_end("CASE", "END_CASE");
            try_char(';');
            restore_case_context(std::move(parent_ctx));
            return true;
        }
        cur_states_.clear();
        cur_guards_.clear();
        cur_trans_.clear();

        parse_branches();

        // Only keep this CASE if at least one branch actually assigned to the state var
        if (!cur_trans_.empty()) commit_case();

        try_kw("END_CASE");
        try_char(';');
        restore_case_context(std::move(parent_ctx));
        return true;
    }

    void parse_branches() {
        while (!at_end()) {
            size_t before = pos_;
            skip_ws_cmts();
            if (peek_kw("END_CASE")) return;

            if (peek_kw("ELSE")) {
                try_kw("ELSE");
                while (!at_end() && !peek_kw("END_CASE")) {
                    size_t before = pos_;
                    skip_stmt_any(false);
                    if (pos_ == before) pos_++;
                }
                return;
            }

            std::string label;
            if (!try_branch_label(label)) {
                if (pos_ == before) pos_++;
                continue;
            }

            std::string state = short_name(label);
            cur_add_state(state);
            parse_branch_body(state);
            if (pos_ == before) pos_++;
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
            size_t before = pos_;
            skip_ws_cmts();
            if (peek_kw("END_CASE") || peek_kw("ELSE")) return;
            if (peek_branch_label()) return;

            if (try_case_stmt()) continue;
            if (try_if_stmt(state)) continue;
            if (try_state_assign(state, false, "")) continue;
            if (try_method_state_change(state, false, "")) continue;
            skip_stmt_any(true);
            if (pos_ == before) pos_++;
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
            size_t before = pos_;
            skip_ws_cmts();
            if (peek_kw("END_IF") || peek_kw("ELSIF") || peek_kw("ELSE")) return;
            if (peek_kw("END_CASE") || peek_branch_label()) return;

            if (try_case_stmt()) continue;
            if (try_if_stmt(state)) continue;
            if (try_state_assign(state, guarded, cond)) continue;
            if (try_method_state_change(state, guarded, cond)) continue;
            skip_stmt_any(true);
            if (pos_ == before) pos_++;
        }
    }

    // Match: <ident that exactly matches one of cur_state_vars_> ':=' QualName ';'
    bool try_state_assign(const std::string& current_state, bool guarded, const std::string& cond) {
        size_t saved = pos_;
        skip_ws_cmts();
        size_t p = pos_;
        std::string lhs;
        if (!parse_lhs_var(src_, p, lhs)) return false;
        pos_ = p;

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

    bool try_method_state_change(const std::string& current_state, bool guarded, const std::string& cond) {
        size_t saved = pos_;
        std::string first;
        if (!try_ident(first)) return false;

        skip_ws_cmts();
        std::string method_name = first;
        if (!at_end() && peek() == '^') {
            pos_++;
            skip_ws_cmts();
            if (at_end() || peek() != '.') { pos_ = saved; return false; }
            pos_++;
            std::string next;
            if (!try_ident(next)) { pos_ = saved; return false; }
            method_name = next;
        } else if (!at_end() && peek() == '.') {
            pos_++;
            std::string next;
            if (!try_ident(next)) { pos_ = saved; return false; }
            method_name = next;
        }

        skip_ws_cmts();
        if (at_end() || peek() != '(') { pos_ = saved; return false; }
        pos_++;

        int depth = 1;
        while (!at_end() && depth > 0) {
            if (peek() == '\'') {
                pos_++;
                while (!at_end() && peek() != '\'') pos_++;
                if (!at_end()) pos_++;
                continue;
            }
            if (peek() == '(') { depth++; pos_++; continue; }
            if (peek() == ')') { depth--; pos_++; continue; }
            pos_++;
        }
        if (depth != 0) { pos_ = saved; return false; }
        try_char(';');

        auto it = method_state_assigns_.find(to_lower(method_name));
        if (it == method_state_assigns_.end()) return true;

        for (const auto& a : it->second) {
            bool matches = std::any_of(cur_state_vars_.begin(), cur_state_vars_.end(),
                                       [&](const std::string& sv) { return iequals(sv, a.lhs); });
            if (!matches) continue;
            cur_record_transition(current_state, a.rhs, cond, guarded);
        }
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

    bool has_graph = false;
    for (const auto& pc : parser.parsed_cases) {
        if (!pc.transitions.empty()) { has_graph = true; break; }
    }
    if (!has_graph) {
        std::cerr << "No state machines found (CASE ... OF with assignments to CASE variable)\n";
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
