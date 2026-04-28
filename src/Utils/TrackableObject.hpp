#pragma once

#include "std/vector.h"  // Include necessary header for std::vector
#include "Message.hpp"

extern "C" void unet_send_dbg_log(const char *text);

/* Set to true by main.cxx right before `new World()` so only the ctor
 * chain of the gameplay-startup path emits trace lines (avoids
 * flooding the UART during boot when every menu / UI element
 * registers a TrackableObject). Cleared after gameplay is up. */
extern bool g_TO_trace;

/**
 * @brief Template structure for objects that can be tracked.
 * @tparam T The type of the trackable object.
 */
template <typename T>
struct TrackableObject : public IMessageHandler
{
    inline static std::vector<T*> objects; /**< Static vector to store pointers to trackable objects. */

    /**
     * @brief Constructor for TrackableObject.
     *
     * Registers the object in the objects vector upon creation.
     */
    TrackableObject()
    {
        if (g_TO_trace) unet_send_dbg_log("TO_pre_pb");
        objects.push_back(static_cast<T*>(this));
        if (g_TO_trace) unet_send_dbg_log("TO_post_pb");
    }

    /**
     * @brief Virtual destructor for TrackableObject.
     *
     * Unregisters the object from the objects vector upon destruction.
     */
    virtual ~TrackableObject()
    {
        auto it = std::find(objects.begin(), objects.end(), this);
        if (it != objects.end())
        {
            objects.erase(it);
        }
    }

    /**
     * @brief Finds the first object in the objects vector.
	 * @return First item
     */
    static T* FirstOrDefault()
    {
        for (T* obj : objects)
        {
            if (obj != nullptr)
            {
                return obj;
            }
        }

		return nullptr;
    }

    /**
     * @brief Finds the first object in the objects vector that satisfies a condition.
     * @tparam ConditionLambda The type of the lambda function specifying the condition.
     * @param condition The lambda function specifying the condition.
	 * @return First found item
     */
    template <typename ConditionLambda>
    static T* FirstOrDefault(ConditionLambda condition)
    {
        for (T* obj : objects)
        {
            if (obj != nullptr && condition(obj))
            {
                return obj;
            }
        }

		return nullptr;
    }
};
