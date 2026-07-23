#pragma once

#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <glaze/glaze.hpp>
#include <magic_enum.hpp>
#include "Tools.hpp"

namespace glz
{
	template <class T> requires std::is_enum_v<T>
	struct meta<T>
	{
		static constexpr auto value = []<std::size_t... Is>(std::index_sequence<Is...>)
		{
			return glz::enumerate(magic_enum::enum_values<T>()[Is]...);
		}(std::make_index_sequence<magic_enum::enum_count<T>()>());
	};

    template <>
    struct meta<Tools::Nanouble> {
        static constexpr auto value = custom<
            [](Tools::Nanouble& obj, const std::variant<double, std::string>& v) {
                if (std::holds_alternative<std::string>(v)) {
                    const auto& s = std::get<std::string>(v);
                    if (s == "NaN") obj.value = std::numeric_limits<double>::quiet_NaN();
                    else if (s == "Infinity") obj.value = std::numeric_limits<double>::infinity();
                    else if (s == "-Infinity") obj.value = -std::numeric_limits<double>::infinity();
                    else throw std::runtime_error("glz::Json::Deserialize: Invalid double value");
                } else {
                    obj.value = std::get<double>(v);
                }
            },
            [](auto& obj) -> std::variant<double, std::string> {
                if (std::isnan(obj.value)) return std::string("NaN");
                if (std::isinf(obj.value)) return (obj.value > 0.0) ? std::string("Infinity") : std::string("-Infinity");
                return obj.value; 
            }
        >;
    };
}

namespace Tools
{
	class Json
	{
	public:
		// ---------------------------------------------------------------------
		// Serialization
		// ---------------------------------------------------------------------
		template <typename T>
		static std::string Serialize(const T& obj)
		{
			static constexpr glz::opts writerOpts = glz::opts{
				.prettify = true,
			};

			std::string buffer;
			glz::error_ctx ec = glz::write<writerOpts>(obj, buffer);

			if (ec)
			{
				throw std::runtime_error("Serialization failed");
			}

			return buffer;
		}

		template <typename T>
		static std::string SerializeToLine(const T& obj)
		{
			static constexpr glz::opts writerOpts = glz::opts{
				.prettify = false,
			};

			std::string buffer;
			glz::error_ctx ec = glz::write<writerOpts>(obj, buffer);

			if (ec)
			{
				throw std::runtime_error("Serialization failed");
			}

			return buffer;
		}

		// ---------------------------------------------------------------------
		// Deserialization
		// ---------------------------------------------------------------------
		template <typename T>
		static T Deserialize(std::string_view jsonString)
		{
			T obj{};

			static constexpr glz::opts readerOpts = glz::opts{
				.error_on_unknown_keys = false,
			};

			glz::error_ctx ec = glz::read<readerOpts>(obj, jsonString);

			if (ec)
			{
				throw std::runtime_error("Deserialization failed: " + glz::format_error(ec, jsonString));
			}

			return obj;
		}
	};
}
