#pragma once
#include <Siv3D.hpp>
#if SIV3D_PLATFORM(WINDOWS)
#include <Siv3D/Windows/Windows.hpp>
#include <d3d11.h>
#include <wrl/client.h>

/// @brief Nonblocking D3D11 timestamps; results are consumed several frames later.
class GpuFrameTimer
{
public:
	void begin(Texture& target)
	{
		if (!m_context && !initialize(target))
		{
			return;
		}
		m_active = -1;
		for (size_t index = 0; index < m_slots.size(); ++index)
		{
			auto& slot = m_slots[index];
			if (slot.pending)
			{
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
				UINT64 first = 0, last = 0;
				constexpr UINT kFlags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
				if (m_context->GetData(slot.disjoint.Get(), &timing, sizeof(timing), kFlags) == S_OK
					&& m_context->GetData(slot.start.Get(), &first, sizeof(first), kFlags) == S_OK
					&& m_context->GetData(slot.finish.Get(), &last, sizeof(last), kFlags) == S_OK)
				{
					if (!timing.Disjoint && timing.Frequency > 0 && last >= first && slot.sequence > m_lastResultSequence)
					{
						m_lastResultSequence = slot.sequence;
						m_milliseconds = 1000.0 * static_cast<double>(last - first) / timing.Frequency;
					}
					slot.pending = false;
				}
			}
			if (!slot.pending && m_active < 0)
			{
				m_active = static_cast<int>(index);
			}
		}
		if (m_active >= 0)
		{
			auto& slot = m_slots[m_active];
			slot.sequence = ++m_sequence;
			m_context->Begin(slot.disjoint.Get());
			m_context->End(slot.start.Get());
		}
	}

	void end()
	{
		if (m_active < 0)
		{
			return;
		}
		auto& slot = m_slots[m_active];
		m_context->End(slot.finish.Get());
		m_context->End(slot.disjoint.Get());
		slot.pending = true;
		m_active = -1;
	}
	[[nodiscard]] double milliseconds() const { return m_milliseconds; }

private:
	template <class T> using ComPointer = Microsoft::WRL::ComPtr<T>;
	struct Slot
	{
		ComPointer<ID3D11Query> disjoint, start, finish;
		bool pending = false;
		uint64 sequence = 0;
	};
	bool initialize(Texture& target)
	{
		ID3D11Texture2D* texture = target.getD3D11Texture2D();
		if (!texture)
		{
			return false;
		}
		ComPointer<ID3D11Device> device;
		texture->GetDevice(device.GetAddressOf());
		for (auto& slot : m_slots)
		{
			D3D11_QUERY_DESC description{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			if (FAILED(device->CreateQuery(&description, slot.disjoint.ReleaseAndGetAddressOf())))
			{
				return false;
			}
			description.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&description, slot.start.ReleaseAndGetAddressOf()))
				|| FAILED(device->CreateQuery(&description, slot.finish.ReleaseAndGetAddressOf())))
			{
				return false;
			}
		}
		device->GetImmediateContext(m_context.GetAddressOf());
		return true;
	}
	ComPointer<ID3D11DeviceContext> m_context;
	std::array<Slot, 8> m_slots;
	int m_active = -1;
	uint64 m_sequence = 0, m_lastResultSequence = 0;
	double m_milliseconds = -1;
};

#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

/// @brief Nonblocking OpenGL timestamps matching the D3D11 timer's units.
class GpuFrameTimer
{
public:
	~GpuFrameTimer()
	{
		for (const auto& slot : m_slots)
		{
			if (slot.start != 0) { glDeleteQueries(1, &slot.start); }
			if (slot.finish != 0) { glDeleteQueries(1, &slot.finish); }
		}
	}
	void begin([[maybe_unused]] Texture& target)
	{
		m_active = -1;
		for (size_t index = 0; index < m_slots.size(); ++index)
		{
			auto& slot = m_slots[index];
			if (slot.pending)
			{
				GLint available = GL_FALSE;
				glGetQueryObjectiv(slot.finish, GL_QUERY_RESULT_AVAILABLE, &available);
				if (available == GL_TRUE)
				{
					GLuint64 first = 0, last = 0;
					glGetQueryObjectui64v(slot.start, GL_QUERY_RESULT, &first);
					glGetQueryObjectui64v(slot.finish, GL_QUERY_RESULT, &last);
					if (last >= first && slot.sequence > m_lastResultSequence)
					{
						m_lastResultSequence = slot.sequence;
						m_milliseconds = static_cast<double>(last - first) / 1000000.0;
					}
					slot.pending = false;
				}
			}
			if (!slot.pending && m_active < 0) { m_active = static_cast<int>(index); }
		}
		if (m_active >= 0)
		{
			auto& slot = m_slots[m_active];
			if (slot.start == 0) { glGenQueries(1, &slot.start); glGenQueries(1, &slot.finish); }
			slot.sequence = ++m_sequence;
			glQueryCounter(slot.start, GL_TIMESTAMP);
		}
	}
	void end()
	{
		if (m_active < 0) { return; }
		auto& slot = m_slots[m_active];
		glQueryCounter(slot.finish, GL_TIMESTAMP);
		slot.pending = true;
		m_active = -1;
	}
	[[nodiscard]] double milliseconds() const { return m_milliseconds; }
private:
	struct Slot { GLuint start = 0, finish = 0; bool pending = false; uint64 sequence = 0; };
	std::array<Slot, 8> m_slots;
	int m_active = -1;
	uint64 m_sequence = 0, m_lastResultSequence = 0;
	double m_milliseconds = -1;
};
#endif
