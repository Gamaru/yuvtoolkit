#include "YT_Interface.h"
#include "RenderThread.h"

#include "VideoViewList.h"
#include "ColorConversion.h"

#include <assert.h>

RenderThread::RenderThread(YT_Renderer* renderer, VideoViewList* list) : m_Renderer(renderer), 
	m_VideoViewList(list), m_SpeedRatio(1.0f)
{
	moveToThread(this);
}

RenderThread::~RenderThread(void)
{

}

#define DIFF_PTS(x,y) qAbs<int>(((int)x)-((int)y))
void RenderThread::run()
{
	qRegisterMetaType<YT_Frame_Ptr>("YT_Frame_Ptr");
	qRegisterMetaType<YT_Frame_List>("YT_Frame_List");
	qRegisterMetaType<UintList>("UintList");
	qRegisterMetaType<RectList>("RectList");

	QTimer* timer = new QTimer(this);
	connect(timer, SIGNAL(timeout()), this, SLOT(Render()), Qt::DirectConnection);
	timer->start(8);

	exec();
}

void RenderThread::Stop()
{
	quit();
	wait();

	for (int i = 0; i < m_LastRenderFrames.size(); i++) 
	{
		YT_Frame_Ptr& renderFrame = m_LastRenderFrames[i];

		if (renderFrame)
		{
			m_Renderer->Deallocate(renderFrame);
			renderFrame.clear();
		}
	}
	m_LastRenderFrames.clear();
	m_LastSourceFrames.clear();

	m_SceneQueue.clear();
	m_PTSQueue.clear();
	m_SeekingQueue.clear();
 }

void RenderThread::Start()
{
	m_LastPTS = INVALID_PTS;
	m_LastSeeking = false;

	QThread::start();

	m_Timer.start();
}

float RenderThread::GetSpeedRatio()
{
	return m_SpeedRatio;
}

void RenderThread::Render()
{
	WARNING_LOG("Render start since last cycle: %d ms", m_Timer.restart());

	int diffPts = 0;
	if (m_SceneQueue.size()>0)
	{		
		YT_Frame_List newScene = m_SceneQueue.first();
		unsigned int pts = m_PTSQueue.first();
		bool seeking = m_SeekingQueue.first();
		
		m_SceneQueue.removeFirst();
		m_PTSQueue.removeFirst();
		m_SeekingQueue.removeFirst();

		// Compute render speed ratio
		if (pts != INVALID_PTS && m_LastPTS != INVALID_PTS && pts>m_LastPTS)
		{
			diffPts = qAbs<int>(((int)pts)-((int)m_LastPTS));
		}

		m_LastRenderFrames = RenderFrames(newScene, m_LastRenderFrames);
		m_LastSourceFrames = newScene;
		m_LastSeeking = seeking;
		m_LastPTS = pts;
	}

	if (m_LastRenderFrames.size()== 0 && m_LastSourceFrames.size()>0)
	{
		m_LastRenderFrames = RenderFrames(m_LastSourceFrames, m_LastRenderFrames);
	}

	// Create render scene with layout info
	YT_Frame_List renderScene;
	for (int i=0; i<m_LastRenderFrames.size(); i++)
	{
		YT_Frame_Ptr renderFrame = m_LastRenderFrames.at(i);
		int j = m_ViewIDs.indexOf(renderFrame->Info(VIEW_ID).toUInt());
		if (j == -1)
		{
			continue;
		}

		renderFrame->SetInfo(SRC_RECT, m_SrcRects.at(j));
		renderFrame->SetInfo(DST_RECT, m_DstRects.at(j));
		renderScene.append(renderFrame);
	}

	WARNING_LOG("Render prepare scene took: %d ms", m_Timer.restart());
	if (renderScene.size()>0 && renderScene.size() == m_ViewIDs.size())
	{
		m_Renderer->RenderScene(renderScene);
	}
	
	WARNING_LOG("Render render scene took: %d ms", m_Timer.restart());

	// Compute render speed ratio
	int elapsedSinceLastPTS = m_RenderSpeedTimer.elapsed();
	if (diffPts > 0 && elapsedSinceLastPTS>0 && diffPts <= 1000 && elapsedSinceLastPTS<=1000)
	{
		WARNING_LOG("TIME %d, %d", diffPts, elapsedSinceLastPTS);
		m_SpeedRatio = m_SpeedRatio + 0.1f * (diffPts*1.0f/ elapsedSinceLastPTS - m_SpeedRatio);
	}
	m_RenderSpeedTimer.start();
	
	emit sceneRendered(m_LastSourceFrames, m_LastPTS, m_LastSeeking);
}

void RenderThread::RenderScene( YT_Frame_List scene, unsigned int pts, bool seeking )
{
	int siz = scene.size();
	m_SceneQueue.append(scene);
	m_PTSQueue.append(pts);
	m_SeekingQueue.append(seeking);
}

void RenderThread::SetLayout(UintList ids, RectList srcRects, RectList dstRects)
{
	m_ViewIDs = ids;
	m_SrcRects = srcRects;
	m_DstRects = dstRects;
}

YT_Frame_List RenderThread::RenderFrames(YT_Frame_List sourceFrames, YT_Frame_List renderFramesOld)
{
	if (sourceFrames.size()==2)
	{
		int i=0;
	}
	YT_Frame_List renderFramesNew;
	for (int i = 0; i < sourceFrames.size(); i++) 
	{
		YT_Frame_Ptr sourceFrame = sourceFrames.at(i);
		if (!sourceFrame)
		{
			continue;
		}

		unsigned int viewID = sourceFrame->Info(VIEW_ID).toUInt();

		YT_Frame_Ptr renderFrame;		
		// Find existing render frame and extract it
		for (int k = 0; k < renderFramesOld.size(); k++) 
		{
			YT_Frame_Ptr _frame = renderFramesOld.at(k);
			if (_frame && _frame->Info(VIEW_ID).toUInt() == viewID)
			{
				renderFramesOld.removeAt(k);
				renderFrame = _frame;
				break;
			}
		}

		// Deallocate if resolution changed
		if (renderFrame && (sourceFrame->Format()->Width() != 
			renderFrame->Format()->Width() || 
			sourceFrame->Format()->Height() != 
			renderFrame->Format()->Height()))
		{
			m_Renderer->Deallocate(renderFrame);
			renderFrame.clear();
		}

		// Allocate if needed
		if (!renderFrame)
		{
			m_Renderer->Allocate(renderFrame, sourceFrame->Format());
			renderFrame->SetInfo(VIEW_ID, viewID);
		}

		// Render frame
		if (m_Renderer->GetFrame(renderFrame) == YT_OK)
		{
			if (sourceFrame->Format() == renderFrame->Format())
			{
				for (int i=0; i<4; i++)
				{
					size_t len = renderFrame->Format()->PlaneSize(i);
					if (len > 0)
					{
						memcpy(renderFrame->Data(i), sourceFrame->Data(i), len);
					}
				}

			}else
			{
				ColorConversion(*sourceFrame, *renderFrame);
			}

			m_Renderer->ReleaseFrame(renderFrame);
		}

		renderFramesNew.append(renderFrame);
	}

	// Delete old frames
	for (int i=0; i<renderFramesOld.size(); i++)
	{
		YT_Frame_Ptr renderFrame = renderFramesOld.at(i);
		if (renderFrame)
		{
			m_Renderer->Deallocate(renderFrame);
		}
		renderFrame.clear();
	}
	renderFramesOld.clear();

	return renderFramesNew;
}
