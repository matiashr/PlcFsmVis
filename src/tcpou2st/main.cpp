// tcpou2st: Convert TwinCAT .TcPOU XML file to IEC 61131-3 Structured Text
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <string>

#include <libxml/parser.h>
#include <libxml/tree.h>

static std::string node_text(const xmlNode* node) {
    std::string out;
    if (!node) return out;

    for (const xmlNode* child = node->children; child; child = child->next) {
        if (child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE) {
            if (child->content) {
                out += reinterpret_cast<const char*>(child->content);
            }
        }
    }
    return out;
}

static const xmlNode* find_child(const xmlNode* parent, const char* name) {
    if (!parent) return nullptr;
    for (const xmlNode* child = parent->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(child->name, reinterpret_cast<const xmlChar*>(name)) == 0) {
            return child;
        }
    }
    return nullptr;
}

static std::string get_impl_st(const xmlNode* n) {
    const xmlNode* impl = find_child(n, "Implementation");
    if (!impl) return "";

    const xmlNode* st = find_child(impl, "ST");
    if (!st) return "";

    return node_text(st);
}

static std::string get_decl(const xmlNode* n) {
    const xmlNode* decl = find_child(n, "Declaration");
    return decl ? node_text(decl) : "";
}

// Detect IEC 61131-3 END_xxx keyword from declaration text
static std::string detect_end_kw(const std::string& decl) {
    static const std::pair<const char*, const char*> table[] = {
        {"FUNCTION_BLOCK", "END_FUNCTION_BLOCK"},
        {"FUNCTION", "END_FUNCTION"},
        {"PROGRAM", "END_PROGRAM"},
        {"METHOD", "END_METHOD"},
        {"PROPERTY", "END_PROPERTY"},
        {"ACTION", "END_ACTION"},
        {"INTERFACE", "END_INTERFACE"},
        {"TYPE", "END_TYPE"},
    };
    std::string lo = decl;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& [kw, end] : table) {
        std::string lkw = kw;
        std::transform(lkw.begin(), lkw.end(), lkw.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        size_t p = lo.find(lkw);
        while (p != std::string::npos) {
            size_t after = p + lkw.size();
            bool ok_before = (p == 0 || (!isalnum((unsigned char)lo[p - 1]) && lo[p - 1] != '_'));
            bool ok_after = (after >= lo.size() || (!isalnum((unsigned char)lo[after]) && lo[after] != '_'));
            if (ok_before && ok_after) return end;
            p = lo.find(lkw, p + 1);
        }
    }
    return "END_UNKNOWN";
}

static void rtrim(std::string& s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
}

static void emit_accessor(const xmlNode* acc, std::ostream& out) {
    std::string kw = reinterpret_cast<const char*>(acc->name);
    std::transform(kw.begin(), kw.end(), kw.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    std::string decl = get_decl(acc);
    std::string impl = get_impl_st(acc);
    rtrim(decl);
    rtrim(impl);

    out << kw << "\n";
    if (!decl.empty()) out << decl << "\n";
    if (!impl.empty()) out << impl << "\n";
    out << "END_" << kw << "\n";
}

static void emit_property(const xmlNode* prop, std::ostream& out) {
    std::string decl = get_decl(prop);
    rtrim(decl);
    if (!decl.empty()) out << decl << "\n";

    for (const xmlNode* c = prop->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(c->name, reinterpret_cast<const xmlChar*>("Get")) == 0 ||
            xmlStrcmp(c->name, reinterpret_cast<const xmlChar*>("Set")) == 0) {
            emit_accessor(c, out);
        }
    }
    out << "END_PROPERTY\n\n";
}

static void emit_method(const xmlNode* method, std::ostream& out) {
    std::string decl = get_decl(method);
    std::string impl = get_impl_st(method);
    rtrim(decl);
    rtrim(impl);

    if (!decl.empty()) out << decl << "\n";
    if (!impl.empty()) out << impl << "\n";
    out << "END_METHOD\n\n";
}

static void emit_pou(const xmlNode* pou, std::ostream& out) {
    std::string decl = get_decl(pou);
    std::string impl = get_impl_st(pou);
    rtrim(decl);
    rtrim(impl);

    std::string end_kw = detect_end_kw(decl);

    if (!decl.empty()) out << decl << "\n";
    if (!impl.empty()) out << impl << "\n";
    out << end_kw << "\n\n";

    for (const xmlNode* c = pou->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(c->name, reinterpret_cast<const xmlChar*>("Method")) == 0) emit_method(c, out);
        if (xmlStrcmp(c->name, reinterpret_cast<const xmlChar*>("Property")) == 0) emit_property(c, out);
    }
}

static const xmlNode* find_pou(const xmlNode* n) {
    if (!n) return nullptr;
    if (n->type == XML_ELEMENT_NODE &&
        xmlStrcmp(n->name, reinterpret_cast<const xmlChar*>("POU")) == 0) {
        return n;
    }
    for (const xmlNode* c = n->children; c; c = c->next) {
        const xmlNode* r = find_pou(c);
        if (r) return r;
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: tcpou2st <file.TcPOU> [output.st]\n";
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "Cannot open: " << argv[1] << "\n";
        return 1;
    }
    std::string src((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());

    xmlDocPtr doc = xmlReadMemory(
        src.c_str(), static_cast<int>(src.size()), argv[1], nullptr,
        XML_PARSE_RECOVER | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        std::cerr << "Failed to parse XML: " << argv[1] << "\n";
        return 1;
    }

    const xmlNode* root = xmlDocGetRootElement(doc);
    const xmlNode* pou = find_pou(root);
    if (!pou) {
        std::cerr << "No POU element found\n";
        xmlFreeDoc(doc);
        return 1;
    }

    if (argc >= 3) {
        std::ofstream out(argv[2]);
        if (!out) {
            std::cerr << "Cannot write: " << argv[2] << "\n";
            xmlFreeDoc(doc);
            return 1;
        }
        emit_pou(pou, out);
    } else {
        emit_pou(pou, std::cout);
    }

    xmlFreeDoc(doc);
    return 0;
}
