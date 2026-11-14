# B03：识别码解码与相机位姿

## 参考核验

核对本机 `C:/Users/11601/project/xyz-vision-lib` 的 `models/2D_Processing/MarkerPoseEstimation.cpp/.hpp`、`property_settings.yml` 与中文属性。目录没有Git元数据，不声明精确外部修订。专有代码仅用于核对行为，没有复制函数体。

参考节点接带内参的图像，输出叠图与多个识别码位姿。ArUco分支使用4x4/5x5/6x6_250；STag通过不可得的`xyz_imgproc`包装固定HD15、纠错7；AprilTag包装固定tag36h11。属性的marker_size提示cm，但执行时直接传入PnP，存在单位说明与计算不一致的风险。三个家族的父帧赋值也不一致，且没有重投影、正深度或平面双解门控。参考MarkerPoseEstimation本身没有标定板聚合；附近的Charuco/Chess/Circle模式属于另一个CalibBoardPoseEstimation节点，不将其混称为单码能力。

## 支持矩阵与依赖

| 家族 | 实际解码后端 | 字典范围 |
| --- | --- | --- |
| ArUco | OpenCV正式二进制字典解码 | 4x4、5x5、6x6、7x7各50/100/250/1000，以及ARUCO_ORIGINAL |
| AprilTag | OpenCV正式AprilTag字典解码 | 16h5、25h9、36h10、36h11 |
| STag | 自有有界轮廓/角点定位、真实极坐标码字采样与固定MIT字典解码，不用ArUco代替 | HD15，纠错7 |

仅支持单个码的独立位姿，多码图像返回多个实例；不推断任意板布局、不从多个无布局码拼造板位姿。没有使用棋盘检测、NCC、识别码ID注入或预先提供四角代替真实像素解码。

OpenCV≥4.7使用main模块objdetect的ArucoDetector；OpenCV4.6使用contrib模块aruco的detectMarkers旧接口。两代共同字典共21个，使用共有的整数型getPredefinedDictionary入口；不引用4.12才有的MIP36h12而破坏4.6/4.7编译。上游接口核验来自[4.6字典头](https://github.com/opencv/opencv_contrib/blob/4.6.0/modules/aruco/include/opencv2/aruco/dictionary.hpp)与[4.7字典头](https://github.com/opencv/opencv/blob/4.7.0/modules/objdetect/include/opencv2/objdetect/aruco_dictionary.hpp)。本机4.12中objdetect要求core/imgproc/calib3d，不需要highgui或dnn；vcpkg端口没有独立objdetect feature，构建层按OpenCV版本寻找objdetect或aruco。STag仅使用[ManfredStoiber/stag固定修订](https://github.com/ManfredStoiber/stag/tree/6214edc03095b6c488d3d95d866aa1a84228e574)的HD15字典数据，保留其MIT声明；原上游检测器及ED/LSD实现不编译、不交付，不将整个库视为已获MIT许可的单一来源。

### STag来源审计边界

固定源的`src/ED/NFA.cpp:62`、`EDLines.cpp:358/540`直接声明从LSD取用NFA和矩形枚举/验证代码。`Utilities.cpp:1271`起的image_double、ntuple_list和gaussian_sampler段与LSD发布源码结构和函数相符；未取得该历史嵌入块可按MIT使用的独立证据。`MyMath.cpp:119`另有Vesa Karvonen保留版权声明的使用条款，也不能仅以仓库顶层MIT将它抹去。因此生产构建不链接原stag目标、不编其任何cpp，也不分发ED目录；仅替换NFA不足以清除其余来源风险。

[IPOL原作者发布页](https://www.ipol.im/pub/art/2012/gjmr-lsd/)明确将LSD1.6代码标为AGPL-3.0-or-later。[OpenCV官方历史说明](https://docs.opencv.org/4.x/db/d73/classcv_1_1LineSegmentDetector.html)记载旧版本曾因许可冲突移除LSD实现，之后依据作者另行发布的[MIT binomial_nfa](https://github.com/rafael-grompone-von-gioi/binomial_nfa)恢复。该后续发布不等于给STag内的旧LSD矩形枚举、重采样或其他嵌入代码统一重许可。

生产依赖限定在纯字典数据：HD15包含766个48bit码字及四个预旋转块，共3064项。`ref/marker_generator/HD15.txt`每行首字符对应采样bit0，按LSB顺序解释后与MarkerIDs.h首766项一致，其余项是12bit的循环移位。独立检测器自行实现阈值、轮廓四边形、subpixel角点、透视映射、双线性采样、黑白环和外边界校验、唯一汉明距离不超过7的解码及朝向重排；不复制上游ED/LSD函数体。它支持真实STag HD15图样，不声称达到原ED定位器、椭圆稳定器或遮挡恢复的性能和鲁棒性。

## 算子契约

入口`image.marker_pose`，注册钩子`registerP3Markers`。输入为全分辨率u8灰度/RGB/RGBA图像，以及必填`vision.CameraCalib.v1`。复用主线相机解析器：图像宽高必须与标定一致，针孔K为正焦距、零skew，D接受0/4/5/8/12/14项，单位m，输出统一使用camera.frame。手动标定也必须先形成同一个相机Record，不在节点中维护另一套fx/fy来源。

参数dictionary选择上述字典或STAG_HD15，markerSize为物理外框边长m，thickness是正的位姿表示厚度而非视觉推断厚度。minArea为原图像素面积；minPerimeterRate用于OpenCV轮廓搜索。maxRms、ambiguityGap单位px；rejectAmbiguous默认开启。最多256个解码目标，图像最多4Mi像素，OpenCV未通过字典解码的候选最多4096个。STag限制2Mi像素、图像短边至少32px；三轮阈值处理累计至多8192条轮廓、8Mi轮廓点、2048个不同四边形候选和800万次码字比较。超预算明确resource_limit，不截断成功结果。

输出instances为标准`vision.Poses3D.v1`：position位于码中心，size为[markerSize,markerSize,thickness]，旋转xyzw，pickable=false。classId是实际解码数字的字符串，classLabel是字典。实例id由字典、解码ID及稳定序号组成；同一ID在不同位置出现时保留多个不同实例，不悄悄合并。

输出detections为`image.Markers.v1`，包含字典、相机参考帧、原图宽高、像素单位、解码后的逐目标四角、decodedId、实例id、accepted/reason、rms和ambiguous。没有可用位姿时rms为null。未被解码的普通方形不生成虚构ID；OpenCV候选拒绝数单独记录，STag后端不报告该值时为null。

输出image保留源位深、通道数和RGBA逐像素alpha，仅绘制识别码边框；原图不被修改。成功与质量拒绝使用不同边框颜色。输入scale必须为1，previewable=false，不在缩放图上复用原图K/D。

## 位姿与拒绝语义

已解码四角统一按真实码朝向TL/TR/BR/BL。物体坐标为`[-s/2,+s/2,0]`、`[+s/2,+s/2,0]`、`[+s/2,-s/2,0]`、`[-s/2,-s/2,0]`，局部X向右、Y向上、Z向外。相机坐标遵循针孔X向右、Y向下、Z向前，因此正对相机的码通常绕X旋转π，不能把单位旋转当成正面真值。

统一使用IPPE_SQUARE的平面双解。每个解验证旋转、平移及所有物体角点的相机深度为有限正数，并以原K/D重新投影求像素RMS。按RMS排序；超maxRms拒绝。两解RMS差不超过ambiguityGap，且旋转矩阵差的Frobenius范数大于1e-3时标为ambiguous；rejectAmbiguous开启时不输出该码的三维位姿。关闭后仍保留ambiguous提示，不把小RMS解释为唯一解或生产置信度。

没有解码目标是有效空结果。已解码但面积/几何、正深度、重投影或双解质量不满足要求时，保留该目标的decodedId/四角及明确拒绝原因，不输出伪成功Pose。相机契约错误、非支持位深、缩放输入或资源上限属于节点诊断。外部后端若返回非法ID、非四角、图外或非有限角点，明确报告invalid_output。

解码调用前后与逐目标PnP阶段检查取消，最终三个输出统一提交；不发布取消后的部分实例。第三方检测调用内部不承诺逐像素抢占，受明确图像预算约束。同输入按解码ID、原图四角排序，稳定生成目标序号；不承诺目标移动后序号保持身份跟踪语义。

## 回归边界

永久测试用真实字典生成像素码，再按独立已知米制姿态、K和非零D投影，检验四个参考要求字典的真实ID、平移、旋转、元数据与重复稳定性；渲染以像素外边界而非像素中心作为物理外框，避免半像素尺度偏差。另测质量/双解拒绝、不匹配字典、同ID多实例、候选预算、RGBAalpha和上游不变、空图、非法标定/位深/缩放及取消原子性。

`examples/p3-stag-hd15.png`是上述固定字典HD15.txt第0项按图样格式几何渲染的1000×1000图样，含100px静区与约800px外框。STag永久回归从真实像素执行透视与K/D真值渲染，核对ID0、四个90°旋转下的完整姿态和上游不变，不注入解码ID或角点；另验证7bit错误可纠正、选定8bit错误样本拒绝、普通黑方形不冒充码、密集轮廓/图像预算与取消不提交结果。

独立Windows/OpenCV4.12小程序只编译自有p3_stag.cpp并读取字典，已实测原图及透视图共8个四方向用例，透视平移误差约1.65mm、旋转矩阵误差约0.018、重投影RMS约0.56px；记录在`build/stag-independent-review/latest.log`。原上游全库probe不作为正式许可准入或生产实现通过依据。完整算子与新版依赖的统一构建执行结果另由本批验证记录给出。
