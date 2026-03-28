#pragma once
// GameTimer.h  — High-resolution frame timer (Frank Luna style, self-contained)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class GameTimer
{
public:
    GameTimer()
        : mSecondsPerCount(0), mDeltaTime(-1), mBaseTime(0),
          mPausedTime(0), mStopTime(0), mPrevTime(0), mCurrTime(0),
          mStopped(false)
    {
        __int64 countsPerSec = 0;
        QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
        mSecondsPerCount = 1.0 / (double)countsPerSec;
    }

    // Total time elapsed since Reset(), not counting paused time
    float TotalTime() const
    {
        if (mStopped)
            return (float)(((mStopTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
        return (float)(((mCurrTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
    }

    float DeltaTime() const { return (float)mDeltaTime; }

    void Reset()
    {
        __int64 currTime = 0;
        QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
        mBaseTime  = currTime;
        mPrevTime  = currTime;
        mStopTime  = 0;
        mStopped   = false;
    }

    void Start()
    {
        if (mStopped)
        {
            __int64 startTime = 0;
            QueryPerformanceCounter((LARGE_INTEGER*)&startTime);
            mPausedTime += (startTime - mStopTime);
            mPrevTime    = startTime;
            mStopTime    = 0;
            mStopped     = false;
        }
    }

    void Stop()
    {
        if (!mStopped)
        {
            __int64 currTime = 0;
            QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
            mStopTime = currTime;
            mStopped  = true;
        }
    }

    void Tick()
    {
        if (mStopped) { mDeltaTime = 0; return; }

        __int64 currTime = 0;
        QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
        mCurrTime = currTime;

        mDeltaTime = (mCurrTime - mPrevTime) * mSecondsPerCount;
        mPrevTime  = mCurrTime;

        if (mDeltaTime < 0.0) mDeltaTime = 0.0;
    }

private:
    double  mSecondsPerCount;
    double  mDeltaTime;
    __int64 mBaseTime, mPausedTime, mStopTime, mPrevTime, mCurrTime;
    bool    mStopped;
};
