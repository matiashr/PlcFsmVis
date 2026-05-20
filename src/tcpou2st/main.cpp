// tcpou2st: Convert TwinCAT .TcPOU XML file to IEC 61131-3 Structured Text
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Minimal XML tree
// ---------------------------------------------------------------------------

struct XmlNode {
    std::string              tag;
    std::map<std::string,std::string> attrs;
    std::string              cdata;
    std::vector<XmlNode>     children;
};

class XmlParser {
    const std::string& s_;
    size_t             pos_;

    void skip_ws() {
        while (pos_ < s_.size() && isspace((unsigned char)s_[pos_])) pos_++;
    }

    std::string read_name() {
        size_t start = pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.') pos_++;
            else break;
        }
        return s_.substr(start, pos_ - start);
    }

    std::string read_attr_value() {
        char q = s_[pos_++];
        size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] != q) pos_++;
        std::string v = s_.substr(start, pos_ - start);
        if (pos_ < s_.size()) pos_++;
        return v;
    }

    void skip_to(const std::string& marker) {
        size_t p = s_.find(marker, pos_);
        pos_ = (p == std::string::npos) ? s_.size() : p + marker.size();
    }

public:
    explicit XmlParser(const std::string& src) : s_(src), pos_(0) {}

    XmlNode parse_document() {
        // Skip <?xml ...?>
        skip_ws();
        while (pos_ + 1 < s_.size() && s_[pos_] == '<' && s_[pos_+1] == '?')
            skip_to("?>");
        skip_ws();
        return parse_element();
    }

    XmlNode parse_element() {
        XmlNode node;
        skip_ws();
        if (pos_ >= s_.size() || s_[pos_] != '<') return node;
        pos_++;  // '<'

        if (pos_ < s_.size() && s_[pos_] == '!') {
            // comment or CDATA at top level - skip
            skip_to(">");
            return node;
        }

        node.tag = read_name();
        if (node.tag.empty()) { skip_to(">"); return node; }

        // attributes
        skip_ws();
        while (pos_ < s_.size() && s_[pos_] != '>' && s_[pos_] != '/') {
            std::string aname = read_name();
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '=') {
                pos_++;
                skip_ws();
                if (pos_ < s_.size() && (s_[pos_] == '"' || s_[pos_] == '\''))
                    node.attrs[aname] = read_attr_value();
            }
            skip_ws();
        }

        if (pos_ < s_.size() && s_[pos_] == '/') { pos_ += 2; return node; }  // />
        if (pos_ < s_.size()) pos_++;  // '>'

        // content
        while (pos_ < s_.size()) {
            skip_ws();
            if (pos_ + 1 < s_.size() && s_[pos_] == '<' && s_[pos_+1] == '/') {
                // end tag
                skip_to(">");
                break;
            }
            if (pos_ + 8 < s_.size() && s_.compare(pos_, 9, "<![CDATA[") == 0) {
                pos_ += 9;
                size_t end = s_.find("]]>", pos_);
                if (end == std::string::npos) { pos_ = s_.size(); break; }
                node.cdata += s_.substr(pos_, end - pos_);
                pos_ = end + 3;
                continue;
            }
            if (pos_ + 3 < s_.size() && s_.compare(pos_, 4, "<!--") == 0) {
                skip_to("-->");
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '<') {
                XmlNode child = parse_element();
                if (!child.tag.empty()) node.children.push_back(std::move(child));
            } else {
                while (pos_ < s_.size() && s_[pos_] != '<') pos_++;
            }
        }
        return node;
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string get_impl_st(const XmlNode& n) {
    for (const auto& c : n.children)
        if (c.tag == "Implementation")
            for (const auto& ic : c.children)
                if (ic.tag == "ST") return ic.cdata;
    return "";
}

static std::string get_decl(const XmlNode& n) {
    for (const auto& c : n.children)
        if (c.tag == "Declaration") return c.cdata;
    return "";
}

// Detect IEC 61131-3 END_xxx keyword from declaration text
static std::string detect_end_kw(const std::string& decl) {
    static const std::pair<const char*, const char*> table[] = {
        {"FUNCTION_BLOCK", "END_FUNCTION_BLOCK"},
        {"FUNCTION",       "END_FUNCTION"},
        {"PROGRAM",        "END_PROGRAM"},
        {"METHOD",         "END_METHOD"},
        {"PROPERTY",       "END_PROPERTY"},
        {"ACTION",         "END_ACTION"},
        {"INTERFACE",      "END_INTERFACE"},
        {"TYPE",           "END_TYPE"},
    };
    std::string lo = decl;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    for (const auto& [kw, end] : table) {
        std::string lkw = kw;
        std::transform(lkw.begin(), lkw.end(), lkw.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        size_t p = lo.find(lkw);
        while (p != std::string::npos) {
            size_t after = p + lkw.size();
            bool ok_before = (p == 0 || (!isalnum((unsigned char)lo[p-1]) && lo[p-1] != '_'));
            bool ok_after  = (after >= lo.size() || (!isalnum((unsigned char)lo[after]) && lo[after] != '_'));
            if (ok_before && ok_after) return end;
            p = lo.find(lkw, p + 1);
        }
    }
    return "END_UNKNOWN";
}

static void rtrim(std::string& s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
}

// ---------------------------------------------------------------------------
// ST reconstruction
// ---------------------------------------------------------------------------

static void emit_accessor(const XmlNode& acc, std::ostream& out) {
    // acc.tag is "Get" or "Set"
    std::string kw = acc.tag;
    std::transform(kw.begin(), kw.end(), kw.begin(),
                   [](unsigned char c){ return std::toupper(c); });

    std::string decl = get_decl(acc);
    std::string impl = get_impl_st(acc);
    rtrim(decl); rtrim(impl);

    out << kw << "\n";
    if (!decl.empty()) out << decl << "\n";
    if (!impl.empty()) out << impl << "\n";
    out << "END_" << kw << "\n";
}

static void emit_property(const XmlNode& prop, std::ostream& out) {
    std::string decl = get_decl(prop);
    rtrim(decl);
    if (!decl.empty()) out << decl << "\n";

    for (const auto& c : prop.children) {
        if (c.tag == "Get" || c.tag == "Set") {
            emit_accessor(c, out);
        }
    }
    out << "END_PROPERTY\n\n";
}

static void emit_method(const XmlNode& method, std::ostream& out) {
    std::string decl = get_decl(method);
    std::string impl = get_impl_st(method);
    rtrim(decl); rtrim(impl);

    if (!decl.empty()) out << decl << "\n";
    if (!impl.empty()) out << impl << "\n";
    out << "END_METHOD\n\n";
}

static void emit_pou(const XmlNode& pou, std::ostream& out) {
    std::string decl = get_decl(pou);
    std::string impl = get_impl_st(pou);
    rtrim(decl); rtrim(impl);

    std::string end_kw = detect_end_kw(decl);

    if (!decl.empty()) out << decl << "\n";
    if (!impl.empty()) out << impl << "\n";
    out << end_kw << "\n\n";

    for (const auto& c : pou.children) {
        if (c.tag == "Method")   emit_method(c, out);
        if (c.tag == "Property") emit_property(c, out);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: tcpou2st <file.TcPOU> [output.st]\n";
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::cerr << "Cannot open: " << argv[1] << "\n"; return 1; }
    std::string src((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    XmlParser parser(src);
    XmlNode root = parser.parse_document();

    // Find POU node (may be nested inside TcPlcObject)
    auto find_pou = [](const XmlNode& n, auto& self) -> const XmlNode* {
        if (n.tag == "POU") return &n;
        for (const auto& c : n.children) {
            const XmlNode* r = self(c, self);
            if (r) return r;
        }
        return nullptr;
    };
    const XmlNode* pou = find_pou(root, find_pou);
    if (!pou) { std::cerr << "No POU element found\n"; return 1; }

    if (argc >= 3) {
        std::ofstream out(argv[2]);
        if (!out) { std::cerr << "Cannot write: " << argv[2] << "\n"; return 1; }
        emit_pou(*pou, out);
    } else {
        emit_pou(*pou, std::cout);
    }
    return 0;
}
