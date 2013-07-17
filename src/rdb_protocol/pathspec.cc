#include "rdb_protocol/pathspec.hpp"

namespace ql {

pathspec_t::pathspec_t(const pathspec_t &other) {
    init_from(other);
}

pathspec_t& pathspec_t::operator=(const pathspec_t &other) {
    init_from(other);
    return *this;
}

pathspec_t::pathspec_t(const std::string &_str, term_t *_creator)
    : type(STR), str(new std::string(_str)), creator(_creator) { }

pathspec_t::pathspec_t(const std::map<std::string, pathspec_t> &_map, term_t *_creator)
    : type(MAP), map(new std::map<std::string, pathspec_t>(_map)), creator(_creator) { }

pathspec_t::pathspec_t(counted_t<const datum_t> datum, term_t *_creator)
    : creator(_creator)
{
    if (datum->get_type() == datum_t::R_STR) {
        type = STR;
        str = new std::string(datum->as_str());
    } else if (datum->get_type() == datum_t::R_ARRAY) {
        type = VEC;
        vec = new std::vector<pathspec_t>;
        for (size_t i = 0; i < datum->size(); ++i) {
            vec->push_back(pathspec_t(datum->get(i), creator));
        }
    } else if (datum->get_type() == datum_t::R_OBJECT) {
        scoped_ptr_t<std::vector<pathspec_t> > local_vec(new std::vector<pathspec_t>);
        scoped_ptr_t<std::map<std::string, pathspec_t> > local_map(new std::map<std::string, pathspec_t>);
        for (auto it = datum->as_object().begin();
             it != datum->as_object().end(); ++it) {
            if (it->second->get_type() == datum_t::R_BOOL &&
                it->second->as_bool() == true) {
                local_vec->push_back(pathspec_t(it->first, creator));
            } else {
                local_map->insert(std::make_pair(it->first, pathspec_t(it->second, creator)));
            }
        }

        if (local_vec->empty()) {
            type = MAP;
            map = local_map.release();
        } else {
            type = VEC;
            vec = local_vec.release();
            if (!local_map->empty()) {
                vec->push_back(pathspec_t(*local_map, creator));
            }
        }
    } else {
        rfail_target(creator, base_exc_t::GENERIC, "Invalid path argument of `%s` to %s.",
                datum->print().c_str(), creator->name());
    }

    if (type == VEC && vec->size() == 1) {
        *this = (*vec)[0];
    }
}

pathspec_t::~pathspec_t() {
    switch (type) {
    case STR:
        delete str;
        break;
    case VEC:
        delete vec;
        break;
    case MAP:
        delete map;
        break;
    default:
        unreachable();
    }
}

/* Limit the datum to only the paths specified by the pathspec. */
counted_t<const datum_t> project(counted_t<const datum_t> datum,
        const pathspec_t &pathspec, recurse_flag_t recurse) {
    if (datum->get_type() == datum_t::R_ARRAY && recurse == RECURSE) {
        scoped_ptr_t<datum_t> res(new datum_t(datum_t::R_ARRAY));
        for (size_t i = 0; i < datum->size(); ++i) {
            res->add(project(datum->get(i), pathspec, DONT_RECURSE));
        }
        return counted_t<const datum_t>(res.release());
    } else {
        scoped_ptr_t<datum_t> res(new datum_t(datum_t::R_OBJECT));
        if (const std::string *str = pathspec.as_str()) {
            if (counted_t<const datum_t> val = datum->get(*str, NOTHROW)) {
                // This bool indicates if things were clobbered. We're fine
                // with things being clobbered so we ignore it.
                UNUSED bool clobbered = res->add(*str, val, CLOBBER);
            }
        } else if (const std::vector<pathspec_t> *vec = pathspec.as_vec()) {
            for (auto it = vec->begin(); it != vec->end(); ++it) {
                counted_t<const datum_t> sub_result = project(datum, *it, recurse);
                for (auto jt = sub_result->as_object().begin();
                     jt != sub_result->as_object().end(); ++jt) {
                    UNUSED bool clobbered = res->add(jt->first, jt->second, CLOBBER);
                }
            }
        } else if (const std::map<std::string, pathspec_t> *map = pathspec.as_map()) {
            for (auto it = map->begin(); it != map->end(); ++it) {
                if (counted_t<const datum_t> val = datum->get(it->first, NOTHROW)) {
                    try {
                        counted_t<const datum_t> sub_result = project(val, it->second, RECURSE);
                        /* We know we're clobbering, that's the point. */
                        UNUSED bool clobbered = res->add(it->first, sub_result, CLOBBER);
                    } catch (const datum_exc_t &e) {
                        //do nothing
                    }
                }
            }
        } else {
            unreachable();
        }
        return counted_t<const datum_t>(res.release());
    }
}

void pathspec_t::init_from(const pathspec_t &other) {
    type = other.type;
    switch (type) {
    case STR:
        str = new std::string(*other.str);
        break;
    case VEC:
        vec = new std::vector<pathspec_t>(*other.vec);
        break;
    case MAP:
        map = new std::map<std::string, pathspec_t>(*other.map);
        break;
    default:
        unreachable();
    }
    creator = other.creator;
}

void unproject_helper(datum_t *datum,
        const pathspec_t &pathspec, recurse_flag_t recurse) {
    if (const std::string *str = pathspec.as_str()) {
        UNUSED bool key_was_deleted = datum->delete_key(*str);
    } else if (const std::vector<pathspec_t> *vec = pathspec.as_vec()) {
        for (auto it = vec->begin(); it != vec->end(); ++it) {
            /* This is all some annoying bullshit caused by the fact that
             * counted_t<datum_t> won't automatically convert to
             * counted_t<const datum_t>. */
            unproject_helper(datum, *it, recurse);
        }
    } else if (const std::map<std::string, pathspec_t> *map = pathspec.as_map()) {
        for (auto it = map->begin(); it != map->end(); ++it) {
            if (counted_t<const datum_t> val = datum->get(it->first, NOTHROW)) {
                counted_t<const datum_t> sub_result = unproject(val, it->second, RECURSE);
                /* We know we're clobbering, that's the point. */
                UNUSED bool clobbered = datum->add(it->first, sub_result, CLOBBER);
            }
        }
    } else {
        unreachable();
    }
}

/* Limit the datum to only the paths not specified by the pathspec. */
counted_t<const datum_t> unproject(counted_t<const datum_t> datum,
        const pathspec_t &pathspec, recurse_flag_t recurse) {
    if (datum->get_type() == datum_t::R_ARRAY && recurse == RECURSE) {
        scoped_ptr_t<datum_t> res(new datum_t(datum_t::R_ARRAY));
        for (size_t i = 0; i < datum->size(); ++i) {
            res->add(unproject(datum->get(i), pathspec, DONT_RECURSE));
        }
        return counted_t<const datum_t>(res.release());
    } else {
        scoped_ptr_t<datum_t> res(new datum_t(datum->as_object()));
        unproject_helper(res.get(), pathspec, recurse);
        return counted_t<const datum_t>(res.release());
    }
}

/* Return whether or not ALL of the paths in the pathspec exist in the datum. */
bool contains(counted_t<const datum_t> datum,
        const pathspec_t &pathspec) {
    bool res = true;
    if (const std::string *str = pathspec.as_str()) {
        if (!(res &= datum->get(*str, NOTHROW).has())) {
            return res;
        }
    } else if (const std::vector<pathspec_t> *vec = pathspec.as_vec()) {
        for (auto it = vec->begin(); it != vec->end(); ++it) {
            if (!(res &= contains(datum, *it))) {
                return res;
            }
        }
    } else if (const std::map<std::string, pathspec_t> *map = pathspec.as_map()) {
        for (auto it = map->begin(); it != map->end(); ++it) {
            if (counted_t<const datum_t> val = datum->get(it->first, NOTHROW)) {
                if (!(res &= contains(val, it->second))) {
                    return res;
                }
            } else {
                return false;
            }
        }
    } else {
        unreachable();
    }
    return res;
}
} //namespace ql
