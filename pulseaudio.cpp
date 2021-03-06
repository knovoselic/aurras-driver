#include "pulseaudio.h"

PulseAudio::PulseAudio()
{
    // Instead of adding and removing a device, for now we'll be lazy and just reload the whole list
    connect(this, &PulseAudio::sourceAdded, this, &PulseAudio::pa_update_source_list);
    connect(this, &PulseAudio::sourceRemoved, this, &PulseAudio::pa_update_source_list);
    // Unfortunately we do not know which property of the device has been changed, so we'll just
    // set this device's mute state to match the global mute state. In some cases this will not
    // be needed (the mute states will match), but it's less expansive than querying all information
    // about the device and then muting it if needed
    connect(this, &PulseAudio::sourceUpdated, this, &PulseAudio::pa_set_source_mute_to_master_mute_by_index);

    connect(this, &PulseAudio::sourceOutputAdded, this, &PulseAudio::updateSourceOutputCount);
    connect(this, &PulseAudio::sourceOutputRemoved, this, &PulseAudio::updateSourceOutputCount);
    // I don't think we need to update count on source_output_updated, so we won't connect it for now

    pa_operation *op;
    int result;

    // Create a threaded mainloop API and a connection to the default server
    mainloop = pa_threaded_mainloop_new();
    result = pa_threaded_mainloop_start(mainloop);
    if(result != 0) {
        qFatal("pa_threaded_mainloop_start has failed!");
    }

    mainloop_api = pa_threaded_mainloop_get_api(mainloop);
    context = pa_context_new(mainloop_api, "Aurras");

    qDebug() << "Connecting to PulseAudio server...";
    pa_threaded_mainloop_lock(mainloop);

    pa_context_set_state_callback(context, pa_state_cb, mainloop);
    result = pa_context_connect(context, NULL, PA_CONTEXT_NOFLAGS, NULL);
    if(result != 0) {
        qFatal("pa_context_connect has failed!");
    }

    // We're waiting for pa_context to connect to the running server. Once context state is
    // set to PA_CONTEXT_READY, we're are connected to PA server.
    for(;;) {
        pa_context_state_t context_state = pa_context_get_state(context);

        if (!PA_CONTEXT_IS_GOOD(context_state)) {
            qFatal("Unable to connect, PA context state is BAD: %d", context_state);
            break;
        }
        if (context_state == PA_CONTEXT_READY) break;
        pa_threaded_mainloop_wait(mainloop);
    }

    qDebug() << "Successfully connected to PulseAudio server";

    // Set up event subscription. PA will call our callback (pa_subscribe_cb) any time
    // an event happens that matches our mask - in our case on any source-related event
    pa_context_set_subscribe_callback(context, pa_subscribe_cb, this);
    op = pa_context_subscribe(context, pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT), NULL, NULL);
    Q_ASSERT(op);
    pa_operation_unref(op);
    pa_threaded_mainloop_unlock(mainloop);

    pa_update_source_list();
    updateSourceOutputCount();
}

PulseAudio::~PulseAudio()
{
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_threaded_mainloop_stop(mainloop);
    pa_threaded_mainloop_free(mainloop);
}

void PulseAudio::pa_state_cb(pa_context *c, void *userdata) {
    pa_context_state_t state;
    pa_threaded_mainloop *m = static_cast<pa_threaded_mainloop*>(userdata);

    state = pa_context_get_state(c);
    switch (state) {
    case PA_CONTEXT_UNCONNECTED:
        qDebug() << "PulseAudio state change: PA_CONTEXT_UNCONNECTED";
        break;
    case PA_CONTEXT_CONNECTING:
        qDebug() << "PulseAudio state change: PA_CONTEXT_CONNECTING";
        break;
    case PA_CONTEXT_AUTHORIZING:
        qDebug() << "PulseAudio state change: PA_CONTEXT_AUTHORIZING";
        break;
    case PA_CONTEXT_SETTING_NAME:
        qDebug() << "PulseAudio state change: PA_CONTEXT_SETTING_NAME";
        break;
    case PA_CONTEXT_READY:
        qDebug() << "PulseAudio state change: PA_CONTEXT_READY";
        break;
    case PA_CONTEXT_FAILED:
        qDebug() << "PulseAudio state change: PA_CONTEXT_FAILED";
        Q_ASSERT_X(false, __FUNCTION__, "Connection to PulseAudio has failed.");
        break;
    case PA_CONTEXT_TERMINATED:
        qDebug() << "PulseAudio state change: PA_CONTEXT_TERMINATED";
        break;
    default:
        Q_ASSERT_X(false,
                   __FUNCTION__,
                   QString("We should never reach this state: %1!").arg(state).toStdString().c_str());
        break;
    }

    pa_threaded_mainloop_signal(m, 0);
}

void PulseAudio::pa_subscribe_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    Q_UNUSED(c);

    PulseAudio *instance = static_cast<PulseAudio *>(userdata);
    pa_subscription_event_type_t event_type = static_cast<pa_subscription_event_type_t>(t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
    pa_subscription_event_type_t event_operation = static_cast<pa_subscription_event_type_t>(t & PA_SUBSCRIPTION_EVENT_TYPE_MASK);

    switch(event_type) {
    case PA_SUBSCRIPTION_EVENT_SOURCE: {
        switch(event_operation) {
        case PA_SUBSCRIPTION_EVENT_NEW:
            emit instance->sourceAdded(idx);
            break;
        case PA_SUBSCRIPTION_EVENT_REMOVE:
            emit instance->sourceRemoved(idx);
            break;
        case PA_SUBSCRIPTION_EVENT_CHANGE:
            emit instance->sourceUpdated(idx);
            break;
        default:
            qWarning("Received unexpected event operation %d", event_operation);
            break;
        }
        break;
    }
    case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT: {
        switch(event_operation) {
        case PA_SUBSCRIPTION_EVENT_NEW:
            emit instance->sourceOutputAdded(idx);
            break;
        case PA_SUBSCRIPTION_EVENT_REMOVE:
            emit instance->sourceOutputRemoved(idx);
            break;
        case PA_SUBSCRIPTION_EVENT_CHANGE:
            emit instance->sourceOutputUpdated(idx);
            break;
        default:
            qWarning("Received unexpected event operation %d", event_operation);
            break;
        }
        break;
    }
    default:
        qWarning("Received unexpected event type %d", event_type);
    }
}

void PulseAudio::updateSourceOutputCount() {
    qDebug() << "Source output has been added or removed. Updating source output count...";

    pa_threaded_mainloop_lock(mainloop);
    active_source_output_count = 0;
    pa_operation *op = pa_context_get_source_output_info_list(context, pa_source_output_list_cb, this);
    Q_ASSERT(op);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainloop);
    }
    pa_operation_unref(op);
    pa_threaded_mainloop_unlock(mainloop);

    emit activeSourceOutputCountChanged(active_source_output_count);
    qDebug() << "Active source outputs:" << active_source_output_count;
}

void PulseAudio::pa_source_output_list_cb(pa_context *c, const pa_source_output_info *l, int eol, void *userdata) {
    Q_UNUSED(c);
    PulseAudio *instance = static_cast<PulseAudio*>(userdata);

    if (eol) {
        pa_threaded_mainloop_signal(instance->mainloop, 0);
        return;
    }

    ++instance->active_source_output_count;
    qDebug() << "Index" << l->index <<  "Name" << l->name << "Driver" << l->driver << "Has volume" << l->has_volume;
}

void PulseAudio::setMuteForAllInputDevices(bool muted) {
    master_mute = muted ? 1 : 0;

    for(auto& device : sources) {
        pa_set_source_mute_to_master_mute_by_index(device.index);
    }
}

void PulseAudio::pa_set_source_mute_to_master_mute_by_index(quint32 idx) {
    pa_threaded_mainloop_lock(mainloop);

    pa_operation *op;
    op = pa_context_set_source_mute_by_index(context, idx, master_mute, pa_mute_cb, this);
    Q_ASSERT(op);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainloop);
    }
    pa_operation_unref(op);

    pa_threaded_mainloop_unlock(mainloop);
}

void PulseAudio::pa_mute_cb(pa_context *c, int success, void *userdata) {
    Q_UNUSED(c);

    Q_ASSERT(success);
    qDebug() << "pa_mute_cb" << success << "errno:" << pa_context_errno(c);

    PulseAudio *instance = static_cast<PulseAudio*>(userdata);
    pa_threaded_mainloop_signal(instance->mainloop, 0);
}

void PulseAudio::pa_update_source_list() {
    pa_operation *op;

    pa_threaded_mainloop_lock(mainloop);
    sources.clear();
    op = pa_context_get_source_info_list(context, pa_source_list_cb, this);
    Q_ASSERT(op);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainloop);
    }
    pa_operation_unref(op);
    pa_threaded_mainloop_unlock(mainloop);
}

void PulseAudio::pa_source_list_cb(pa_context *c, const pa_source_info *info, int eol, void *userdata) {
    Q_UNUSED(c);
    PulseAudio *instance = static_cast<PulseAudio*>(userdata);

    if (eol) {
        pa_threaded_mainloop_signal(instance->mainloop, 0);
#ifdef QT_DEBUG
        instance->print_source_list();
#endif
        return;
    }

    instance->sources.append(PulseAudioDevice(info->name, info->index));
}
