// extract_class_name: the class taken from std::source_location::function_name().
#include <gtest/gtest.h>

#include <functional>
#include <source_location>
#include <string>
#include <string_view>

#include "lumen/detail/class_name.hpp"

// At global scope, with a namespace in its return type: this compiler's own
// spelling of such a function. Its result is the function's name; the
// std::string it returns carries it.
std::string name_of_a_global_function_returning_a_string();
std::string name_of_a_global_function_returning_a_string() {
    return std::source_location::current().function_name();
}

namespace lumen {
namespace {

using detail::extract_class_name;

TEST(ClassNameTest, MethodYieldsItsClass) {
    EXPECT_EQ(extract_class_name("void MyClass::method()"), "MyClass");
}

TEST(ClassNameTest, StaticMethodYieldsItsClass) {
    EXPECT_EQ(extract_class_name("static void MyClass::static_method()"), "MyClass");
}

TEST(ClassNameTest, ConstructorYieldsItsClass) {
    EXPECT_EQ(extract_class_name("MyClass::MyClass(int)"), "MyClass");
}

TEST(ClassNameTest, NestedClassYieldsTheInnermostClass) {
    EXPECT_EQ(extract_class_name("void Outer::Inner::deep()"), "Inner");
}

TEST(ClassNameTest, ParameterTypesWithScopeDoNotConfuseIt) {
    EXPECT_EQ(extract_class_name("void Parser::parse(std::string_view, lumen::Core&)"), "Parser");
}

TEST(ClassNameTest, FreeFunctionYieldsEmpty) {
    EXPECT_EQ(extract_class_name("void free_function(int, double)"), "");
}

TEST(ClassNameTest, EmptyInputYieldsEmpty) {
    EXPECT_EQ(extract_class_name(""), "");
}

TEST(ClassNameTest, InputWithoutParenthesisYieldsEmpty) {
    EXPECT_EQ(extract_class_name("MyClass::member"), "");
}

TEST(ClassNameTest, ExtraSpaceBeforeTheQualifiedNameIsIgnored) {
    EXPECT_EQ(extract_class_name("void  A::b()"), "A");
}

TEST(ClassNameTest, NamespaceQualifiedFreeFunctionYieldsTheNamespace) {
    // A known limit of reading the text: a namespace looks like a class.
    EXPECT_EQ(extract_class_name("void ns::f()"), "ns");
}

TEST(ClassNameTest, WorksAtCompileTime) {
    static_assert(extract_class_name("void K::f()") == "K");
}

TEST(ClassNameTest, ParenthesesInsideAParameterTypeAreNotTheParameterList) {
    EXPECT_EQ(extract_class_name("void Detector::b(std::function<void(int)>)"), "Detector");
    EXPECT_EQ(extract_class_name("void Detector::b(std::function<void (int)>)"), "Detector");
    EXPECT_EQ(
        extract_class_name("void __cdecl Detector::b(class std::function<void __cdecl(int)>)"),
        "Detector");
}

TEST(ClassNameTest, TemplateArgumentListsAfterTheSignatureAreSkipped) {
    EXPECT_EQ(extract_class_name("void Detector::c(T) [with T = std::function<void(int)>]"),
              "Detector");
    EXPECT_EQ(extract_class_name("void Detector::c(T) [T = std::function<void (int)>]"),
              "Detector");
    EXPECT_EQ(extract_class_name("void __cdecl Detector::c<class std::function<void "
                                 "__cdecl(int)>>(class std::function<void __cdecl(int)>)"),
              "Detector");
}

TEST(ClassNameTest, ClassTemplateYieldsItsNameWithoutArguments) {
    EXPECT_EQ(extract_class_name("T* Pool<T>::get() [with T = int]"), "Pool");
    EXPECT_EQ(extract_class_name("std::vector<int> Grid::cells() const"), "Grid");
}

TEST(ClassNameTest, LambdaInAMemberFunctionYieldsTheEnclosingClass) {
    EXPECT_EQ(extract_class_name("Detector::d()::<lambda()>"), "Detector");
    EXPECT_EQ(extract_class_name("auto Detector::d()::(anonymous class)::operator()() const"),
              "Detector");
    EXPECT_EQ(extract_class_name(
                  "auto Detector::d()::(lambda at src/detector.cpp:12:5)::operator()() const"),
              "Detector");
    EXPECT_EQ(extract_class_name("auto __cdecl Detector::d::<lambda_1>::operator ()(void) const"),
              "Detector");
}

TEST(ClassNameTest, LambdaInAFreeFunctionYieldsEmpty) {
    EXPECT_EQ(extract_class_name("f()::<lambda()>"), "");
    EXPECT_EQ(extract_class_name("auto f()::(anonymous class)::operator()() const"), "");
}

TEST(ClassNameTest, OperatorSymbolsAreNotTakenForBrackets) {
    EXPECT_EQ(extract_class_name("bool Vec::operator<(const Vec&) const"), "Vec");
    EXPECT_EQ(extract_class_name("Vec& Vec::operator<<=(int)"), "Vec");
    EXPECT_EQ(extract_class_name("auto Vec::operator()(int)"), "Vec");
    EXPECT_EQ(extract_class_name("auto __cdecl Vec::operator ()(int)"), "Vec");
    EXPECT_EQ(extract_class_name("Vec::operator bool() const"), "Vec");
    EXPECT_EQ(extract_class_name("bool operator<(const Vec&, const Vec&)"), "");
}

TEST(ClassNameTest, LocalClassAndAnonymousNamespaceAreHandled) {
    EXPECT_EQ(extract_class_name("void f()::Local::g()"), "Local");
    EXPECT_EQ(extract_class_name("void {anonymous}::Worker::run()"), "Worker");
    EXPECT_EQ(extract_class_name("void (anonymous namespace)::helper()"), "");
}

TEST(ClassNameTest, NamespaceInTheReturnTypeOfAGlobalFreeFunctionYieldsEmpty) {
    // GCC and Clang
    EXPECT_EQ(extract_class_name("std::string text_of()"), "");
    EXPECT_EQ(extract_class_name("std::__cxx11::basic_string<char> text_of()"), "");
    EXPECT_EQ(extract_class_name("std::vector<int> values_of()"), "");
    EXPECT_EQ(extract_class_name("const std::map<std::string, int>& table_of()"), "");
    // MSVC: the calling convention stands between the return type and the name.
    EXPECT_EQ(extract_class_name("class std::basic_string<char,struct std::char_traits<char>,class "
                                 "std::allocator<char> > __cdecl text_of(void)"),
              "");
    EXPECT_EQ(extract_class_name("class std::vector<int,class std::allocator<int> > __cdecl "
                                 "values_of(void)"),
              "");
}

TEST(ClassNameTest, NamespaceInTheReturnTypeOfAMemberFunctionDoesNotHideTheClass) {
    EXPECT_EQ(extract_class_name("std::string Detector::name() const"), "Detector");
    EXPECT_EQ(extract_class_name("class std::basic_string<char,struct std::char_traits<char>,class "
                                 "std::allocator<char> > __cdecl Detector::name(void) const"),
              "Detector");
    EXPECT_EQ(extract_class_name("static std::vector<int> Detector::values()"), "Detector");
}

TEST(ClassNameTest, TrailingReturnTypeIsNotPartOfTheName) {
    EXPECT_EQ(extract_class_name("auto Detector::name() -> std::string"), "Detector");
    EXPECT_EQ(extract_class_name("auto text_of() -> std::string"), "");
    EXPECT_EQ(extract_class_name("auto values_of() -> std::vector<int>"), "");
}

TEST(ClassNameTest, ConversionOperatorToAScopedTypeYieldsItsClass) {
    EXPECT_EQ(extract_class_name("Vec::operator std::string() const"), "Vec");
    EXPECT_EQ(extract_class_name("__cdecl Vec::operator class std::basic_string<char,struct "
                                 "std::char_traits<char>,class std::allocator<char> >(void) const"),
              "Vec");
}

struct SignatureProbe {
    [[nodiscard]] static std::string_view with_function_parameter(
        const std::function<void(int)>& /*callback*/) {
        return std::source_location::current().function_name();
    }
    [[nodiscard]] static std::string_view from_a_lambda() {
        return [] { return std::string_view(std::source_location::current().function_name()); }();
    }
};

TEST(ClassNameTest, ThisCompilersOwnSpellingsYieldTheClass) {
    const std::string_view with_parameter = SignatureProbe::with_function_parameter([](int) {});
    EXPECT_EQ(extract_class_name(with_parameter), "SignatureProbe") << with_parameter;
    const std::string_view lambda = SignatureProbe::from_a_lambda();
    EXPECT_EQ(extract_class_name(lambda), "SignatureProbe") << lambda;
    const std::string global = name_of_a_global_function_returning_a_string();
    EXPECT_EQ(extract_class_name(global), "") << global;
}

TEST(ClassNameTest, ANameThatEndsInOperatorIsNotTheOperatorKeyword) {
    EXPECT_EQ(extract_class_name("void cooperator::run()"), "cooperator");
    EXPECT_EQ(extract_class_name("void Shop::cooperator()"), "Shop");
}

TEST(ClassNameTest, APartThatIsNotAnIdentifierYieldsNoClassName) {
    // Clang spells some closures "$_0"; that is not a class name.
    EXPECT_EQ(extract_class_name("auto f()::$_0::operator()() const"), "");
}

}  // namespace
}  // namespace lumen
