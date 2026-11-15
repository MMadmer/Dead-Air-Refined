// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// What a module's change of one key of a section does to the sections that inherit it: a section
// copies its parents' keys when it is read, so a later change of a parent reaches them only through
// this. Every section that names the changed one as a parent - directly, or through a chain - and
// does not say the key itself takes the value of its LAST parent that has the key (a later parent
// wins, as `[a] : b, c` reads), or loses the key when none has it. Free of the engine's types, for
// tools/qa/ltx.
//
// `Model` is the config:
//   Name                                   a section's or a key's name
//   Node                                   a section
//   Value                                  a key's value, compared with ==
//   children(n)                            a pointer to a list of the sections naming n as a parent, or null
//   Node* find(n)                          the section of that name, or null
//   parents(node)                          the list of the parents it names, in order
//   bool own(node, key)                    the section says the key itself (or took it away)
//   bool get(node, key, value&)            the section has the key
//   void put(node, key, value, from)       the key takes the value of the parent `from`
//   void take(node, key)                   the key goes

namespace XMS
{
template <class Model>
void PropagateKey(Model& model, const typename Model::Name& changed, const typename Model::Name& key, int depth = 0)
{
    // a chain is never this long; a loop that removing and making sections again could tie ends here
    if (depth >= 64)
        return;
    const auto* children = model.children(changed);
    if (!children)
        return;
    for (size_t i = 0; i < children->size(); ++i)
    {
        const typename Model::Name child = (*children)[i];
        typename Model::Node* node = model.find(child);
        if (!node || model.own(*node, key))
            continue;

        const typename Model::Node* from = nullptr;
        typename Model::Value value{};
        const auto& parents = model.parents(*node);
        for (size_t p = parents.size(); p-- > 0 && !from;)
        {
            const typename Model::Node* parent = model.find(parents[p]);
            if (parent && model.get(*parent, key, value))
                from = parent;
        }

        typename Model::Value current{};
        const bool had = model.get(*node, key, current);
        if (from ? had && current == value : !had)
            continue;
        if (from)
            model.put(*node, key, value, *from);
        else
            model.take(*node, key);
        PropagateKey(model, child, key, depth + 1);
    }
}
} // namespace XMS
