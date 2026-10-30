#include "aov_table.h"
#include <cstdio>
#include <set>
#include <string>
// The AOV table: names, Karma aliases and light path expressions select their rows, and no name selects two rows.
int main() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "aov table: %s\n", what);
            ++failures;
        }
    };
    const auto find = [](const char* name) { return HdUEFindAov(name); };
    expect(find("albedo") && find("albedo")->pathTraced == HDUE_AOV_ALBEDO, "albedo uses the Path Tracer's albedo");
    expect(find("CDA") == find("albedo"), "Karma's diffuse albedo LPE");
    expect(find("ray:hitN") == find("normal") && find("N") == find("normal"), "Karma's N");
    expect(find("C<RD>.*L") == find("combineddiffuse"), "Karma's combined diffuse LPE");
    expect(find("combineddiffuse")->aov ==
               (HDUE_AOV_LIGHTING | (HDUE_LIGHTING_DIFFUSE | HDUE_LIGHTING_INDIRECT_DIFFUSE) << 8),
           "lighting components ride in the request's aov");
    expect(find("combinedvolume")->modes == HdUEPathTraced, "volumes split only path traced");
    expect(!find("") && !find("C") && !find("CryptoObject00") && !find("C<RD>"), "no row for other names");
    std::set<std::string> names;
    for (const auto& row : HdUEAovTable) {
        expect(names.insert(row.name).second, row.name);
        for (std::string_view aliases = row.aliases; !aliases.empty();) {
            const size_t end = aliases.find(' ');
            expect(names.insert(std::string(aliases.substr(0, end))).second, "an alias names two rows");
            aliases = end == std::string_view::npos ? std::string_view() : aliases.substr(end + 1);
        }
        expect((row.kind == HdUEAovKind::Buffer) == (row.buffer != nullptr), "buffer rows name their buffer");
        expect(row.doc && *row.doc, "every row is documented");
    }
    return failures;
}
