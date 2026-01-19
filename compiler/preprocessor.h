#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include <string>
#include <map>
#include <vector>
#include <stack>
#include <set>

class Preprocessor {
public:
    struct Macro {
        std::string name;
        std::string value;
        std::vector<std::string> params;
        bool is_function;
    };

    Preprocessor();

    std::string process(const std::string& filename, bool debug);
    void define(const std::string& name, const std::string& value = "");
    void define(const std::string& name, const std::vector<std::string>& params, const std::string& value);
    void undef(const std::string& name);
    bool isDefined(const std::string& name);
    bool evaluateCondition(const std::string& expr);
    std::vector<std::string> getIncludedFiles() const { return includedFiles_; }
    std::map<std::string, Macro> getMacros() const { return macros_; }

private:
    std::map<std::string, Macro> macros_;
    std::stack<bool> conditionStack_;
    std::stack<bool> skipStack_;
    std::vector<std::string> includedFiles_;
    std::string expandMacros(const std::string& input);
    std::string trim(const std::string& str);
    int evaluateExpression(const std::string& expr);

    bool processDefine(const std::string& line);
    bool processUndef(const std::string& line);
    bool processIf(const std::string& line);
    bool processIfdef(const std::string& line, bool checkDefined);
    bool processElif(const std::string& line);
    bool processElse(const std::string& line);
    bool processEndif(const std::string& line);

    std::vector<std::string> parseMacroArguments(const std::string& argsStr);
};

#endif